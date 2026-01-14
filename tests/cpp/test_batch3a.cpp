// Copyright 2025 CPIM Project
// Batch-3A MVP Test

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>
#include <utility>
#include <vector>

#include "GModel.cuh"
#include "model/gmodel_adapter.h"
#include "model/model_normalizer.h"
#include "model/xcsp_parser.h"
#include "solver/gpu/batch_probe_manager.h"

#ifndef CPIM_PROJECT_ROOT
#define CPIM_PROJECT_ROOT "."
#endif

DEFINE_string(input, CPIM_PROJECT_ROOT "/tests/data/bench/queens-4_ext.xml",
              "Input XCSP3 file path");
DEFINE_int32(num_probes, 10, "Number of probe tasks to run");
DEFINE_bool(compare_stage2, true, "Compare with Stage 2 results");

namespace cpim {

using cpim::model::ModelNormalizer;
using cpim::model::ParserType;
using cpim::model::XcspParser;

std::vector<ProbeTask> CollectProbeTasks(GModel* gmodel, int max_tasks) {
  std::vector<ProbeTask> tasks;
  const int num_vars = gmodel->GetNumVars();
  for (int var = 0; var < num_vars && static_cast<int>(tasks.size()) < max_tasks; ++var) {
    if (gmodel->IsAssigned(var)) continue;
    for (int val = gmodel->GetFirstValue(var);
         val != -1 && static_cast<int>(tasks.size()) < max_tasks;
         val = gmodel->GetNextValue(var, val)) {
      tasks.emplace_back(var, val, tasks.size());
    }
  }
  return tasks;
}

// 分析聚合度：模拟 Batch-3A 第一轮迭代的任务构建
// 统计 world_mask 的 popcount 分布
void AnalyzeAggregationPotential(
    GModel* gmodel,
    const std::vector<ProbeTask>& tasks) {
  const int num_cons = gmodel->GetNumCons();
  const int num_worlds = static_cast<int>(tasks.size());

  LOG(INFO) << "========================================";
  LOG(INFO) << "Aggregation Potential Analysis";
  LOG(INFO) << "========================================";
  LOG(INFO) << "  num_worlds: " << num_worlds;
  LOG(INFO) << "  num_constraints: " << num_cons;

  // 使用 constraint_scopes_cpu 获取约束涉及的变量
  const auto& scopes = gmodel->constraint_scopes_cpu;

  // popcount 分布统计
  std::vector<int> popcount_histogram(33, 0);  // 0-32
  int total_tasks = 0;
  int total_popcount = 0;

  // 模拟第一轮迭代的任务构建
  for (int cid = 0; cid < num_cons; ++cid) {
    u32 world_mask = 0;

    // 获取约束 cid 涉及的变量
    const auto& scope_vars = scopes[cid];

    // 检查每个 world 是否激活了该约束
    for (int w = 0; w < num_worlds; ++w) {
      const int probe_var = tasks[w].var_id;

      // 检查 probe_var 是否在约束 cid 的 scope 中
      for (int var : scope_vars) {
        if (var == probe_var) {
          world_mask |= (1u << w);
          break;
        }
      }
    }

    if (world_mask != 0) {
      const int popcount = __builtin_popcount(world_mask);
      popcount_histogram[popcount]++;
      total_tasks++;
      total_popcount += popcount;
    }
  }

  LOG(INFO) << "  active_constraints (K): " << total_tasks << " / " << num_cons
            << " (" << (100.0 * total_tasks / num_cons) << "%)";
  LOG(INFO) << "  avg_popcount: " << (total_tasks > 0 ? (1.0 * total_popcount / total_tasks) : 0);
  LOG(INFO) << "  total_popcount: " << total_popcount;
  LOG(INFO) << "  theoretical_speedup (vs separate): "
            << (total_tasks > 0 ? (1.0 * total_popcount / total_tasks) : 1.0) << "x";

  LOG(INFO) << "  popcount distribution:";
  for (int i = 1; i <= std::min(num_worlds, 32); ++i) {
    if (popcount_histogram[i] > 0) {
      LOG(INFO) << "    popcount=" << i << ": " << popcount_histogram[i]
                << " constraints (" << (100.0 * popcount_histogram[i] / total_tasks) << "%)";
    }
  }

  // 分析结论
  double avg_popcount = total_tasks > 0 ? (1.0 * total_popcount / total_tasks) : 1.0;
  if (avg_popcount < 1.5) {
    LOG(WARNING) << "  ⚠ Low aggregation potential (avg_popcount < 1.5)";
    LOG(WARNING) << "    Batch-3A unlikely to outperform Stage 2";
  } else if (avg_popcount < 3.0) {
    LOG(INFO) << "  📊 Moderate aggregation potential";
  } else {
    LOG(INFO) << "  ✓ High aggregation potential (avg_popcount >= 3.0)";
  }
  LOG(INFO) << "========================================";
}

std::vector<std::pair<int, int>> RunBatch3A(
    GModel* gmodel,
    const std::vector<ProbeTask>& tasks) {
  Batch3AManager mgr(gmodel, -1, 32);

  for (const auto& t : tasks) {
    mgr.AddTask(t.var_id, t.value);
  }

  std::vector<int> failed_vars, failed_values;
  mgr.Execute(failed_vars, failed_values);

  std::vector<std::pair<int, int>> failed_pairs;
  failed_pairs.reserve(failed_vars.size());
  for (size_t i = 0; i < failed_vars.size(); ++i) {
    failed_pairs.emplace_back(failed_vars[i], failed_values[i]);
  }
  std::sort(failed_pairs.begin(), failed_pairs.end());
  return failed_pairs;
}

std::vector<std::pair<int, int>> RunStage2(
    GModel* gmodel,
    const std::vector<ProbeTask>& tasks) {
  Batch2PersistentManager mgr(gmodel, -1);

  for (const auto& t : tasks) {
    mgr.AddTask(t.var_id, t.value);
  }

  std::vector<int> failed_vars, failed_values;
  mgr.ExecutePersistentBlocks(failed_vars, failed_values);

  std::vector<std::pair<int, int>> failed_pairs;
  failed_pairs.reserve(failed_vars.size());
  for (size_t i = 0; i < failed_vars.size(); ++i) {
    failed_pairs.emplace_back(failed_vars[i], failed_values[i]);
  }
  std::sort(failed_pairs.begin(), failed_pairs.end());
  return failed_pairs;
}

bool TestBatch3A() {
  LOG(INFO) << "========================================";
  LOG(INFO) << "Test: Batch-3A (Constraint Aggregation)";
  LOG(INFO) << "========================================";

  LOG(INFO) << "Loading instance: " << FLAGS_input;
  auto parser = XcspParser::Create(ParserType::kLibXml2);
  auto model_or = parser->Parse(FLAGS_input);
  if (!model_or.ok()) {
    LOG(ERROR) << "Failed to parse model: " << model_or.status();
    return false;
  }

  ModelNormalizer normalizer;
  auto normalized_or = normalizer.Normalize(*model_or);
  if (!normalized_or.ok()) {
    LOG(ERROR) << "Normalization failed: " << normalized_or.status();
    return false;
  }
  const auto& im_model = *normalized_or;

  LOG(INFO) << "Building GModel...";
  auto gmodel_obj = model::GModelAdapter::Build(im_model);
  GModel* gmodel = &gmodel_obj;

  LOG(INFO) << "Model info: "
            << gmodel->GetNumVars() << " vars, "
            << gmodel->GetNumCons() << " constraints, "
            << "max_dom_size=" << gmodel->max_dom_size
            << ", bit_dom_int_size=" << gmodel->GetBitDomIntSize();

  LOG(INFO) << "Running initial GAC propagation (snapshot baseline)...";
  auto gac_stats = gmodel->EnforceGAC_Persistent(false, -1);
  if (gac_stats.inconsistent) {
    LOG(INFO) << "Initial GAC detected inconsistency (UNSAT instance)";
    LOG(INFO) << "✓ Test skipped (UNSAT instance)";
    return true;
  }

  LOG(INFO) << "Initial GAC: " << gac_stats.deletions << " deletions, "
            << gac_stats.iterations << " iterations";

  // Collect probe tasks
  auto tasks = CollectProbeTasks(gmodel, FLAGS_num_probes);
  LOG(INFO) << "Collected " << tasks.size() << " probe tasks";

  if (tasks.empty()) {
    LOG(INFO) << "No probe tasks available (all variables are singletons)";
    LOG(INFO) << "✓ Test skipped (no probes)";
    return true;
  }

  // Check Batch-3A suitability
  Batch3AManager batch3a_check(gmodel, -1, 32);
  LOG(INFO) << "bitSup size per constraint: "
            << batch3a_check.GetBitSupSizePerConstraint() << " bytes";
  LOG(INFO) << "Suitable for Batch-3A: "
            << (batch3a_check.IsSuitableForBatch3A() ? "YES" : "NO");

  // Analyze aggregation potential (first iteration only)
  AnalyzeAggregationPotential(gmodel, tasks);

  // Run Batch-3A
  LOG(INFO) << "Running Batch-3A...";
  auto start_3a = std::chrono::high_resolution_clock::now();
  auto failed_3a = RunBatch3A(gmodel, tasks);
  auto end_3a = std::chrono::high_resolution_clock::now();
  double time_3a_ms = std::chrono::duration<double, std::milli>(end_3a - start_3a).count();

  LOG(INFO) << "Batch-3A: " << failed_3a.size() << " / " << tasks.size()
            << " probes failed in " << time_3a_ms << " ms";

  if (FLAGS_compare_stage2) {
    // Run Stage 2 for comparison
    LOG(INFO) << "Running Stage 2 for comparison...";
    auto start_s2 = std::chrono::high_resolution_clock::now();
    auto failed_s2 = RunStage2(gmodel, tasks);
    auto end_s2 = std::chrono::high_resolution_clock::now();
    double time_s2_ms = std::chrono::duration<double, std::milli>(end_s2 - start_s2).count();

    LOG(INFO) << "Stage 2: " << failed_s2.size() << " / " << tasks.size()
              << " probes failed in " << time_s2_ms << " ms";

    // Compare results
    bool results_match = (failed_3a == failed_s2);

    LOG(INFO) << "========================================";
    LOG(INFO) << "Comparison Results:";
    LOG(INFO) << "  Batch-3A time: " << time_3a_ms << " ms";
    LOG(INFO) << "  Stage 2 time:  " << time_s2_ms << " ms";
    LOG(INFO) << "  Speedup:       " << (time_s2_ms / time_3a_ms) << "x";
    LOG(INFO) << "  Results match: " << (results_match ? "YES ✓" : "NO ✗");
    LOG(INFO) << "========================================";

    if (!results_match) {
      LOG(ERROR) << "Batch-3A results do not match Stage 2!";
      LOG(ERROR) << "Batch-3A failed: " << failed_3a.size();
      LOG(ERROR) << "Stage 2 failed:  " << failed_s2.size();

      // Show first few differences
      int diff_count = 0;
      for (const auto& p : failed_3a) {
        if (std::find(failed_s2.begin(), failed_s2.end(), p) == failed_s2.end()) {
          LOG(ERROR) << "  Batch-3A only: var=" << p.first << ", val=" << p.second;
          if (++diff_count >= 5) break;
        }
      }
      diff_count = 0;
      for (const auto& p : failed_s2) {
        if (std::find(failed_3a.begin(), failed_3a.end(), p) == failed_3a.end()) {
          LOG(ERROR) << "  Stage 2 only: var=" << p.first << ", val=" << p.second;
          if (++diff_count >= 5) break;
        }
      }

      return false;
    }
  }

  LOG(INFO) << "✓ Test passed!";
  return true;
}

}  // namespace cpim

int main(int argc, char* argv[]) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  bool success = cpim::TestBatch3A();

  return success ? 0 : 1;
}
