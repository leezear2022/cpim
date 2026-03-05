// Copyright 2025 CPIM Project
// Unit Test: Batch-2 (Micro-Batch) vs Batch-1 (Cooperative)
//
// 目标：
// 1) 在同一 AC snapshot 上，比较 Batch-1 与 Batch-2 Micro-Batch 的探测结果一致性
// 2) 验证 Batch-2 Micro-Batch 不会污染 GModel 的原始域状态

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <algorithm>
#include <cstring>
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

namespace cpim {

using cpim::model::ModelNormalizer;
using cpim::model::ParserType;
using cpim::model::XcspParser;

template <typename T>
std::vector<T> CopyFromDevice(const T* d_ptr, int count) {
  std::vector<T> host_copy(count);
  cudaError_t err = cudaMemcpy(host_copy.data(), d_ptr,
                                count * sizeof(T),
                                cudaMemcpyDeviceToHost);
  CHECK(err == cudaSuccess) << "cudaMemcpy failed: " << cudaGetErrorString(err);
  return host_copy;
}

template <typename T>
bool ArraysEqual(const T* a, const T* b, int count, const std::string& name) {
  for (int i = 0; i < count; ++i) {
    if (a[i] != b[i]) {
      LOG(ERROR) << name << " mismatch at index " << i
                 << ": expected " << b[i] << ", got " << a[i];
      return false;
    }
  }
  return true;
}

std::vector<ProbeTask> CollectAllProbeTasks(GModel* gmodel) {
  std::vector<ProbeTask> tasks;
  const int num_vars = gmodel->GetNumVars();
  for (int var = 0; var < num_vars; ++var) {
    if (gmodel->IsAssigned(var)) continue;
    for (int val = gmodel->GetFirstValue(var);
         val != -1;
         val = gmodel->GetNextValue(var, val)) {
      tasks.emplace_back(var, val, tasks.size());
    }
  }
  return tasks;
}

std::vector<std::pair<int, int>> RunBatch1(
    GModel* gmodel,
    const std::vector<ProbeTask>& tasks,
    int activation_strategy) {
  BatchProbeManager batch1_mgr(gmodel, static_cast<int>(tasks.size()));
  batch1_mgr.SetActivationStrategy(activation_strategy);

  for (const auto& t : tasks) {
    batch1_mgr.AddTask(t.var_id, t.value);
  }

  std::vector<int> failed_vars;
  std::vector<int> failed_values;
  batch1_mgr.ExecuteBatch(failed_vars, failed_values);

  std::vector<std::pair<int, int>> failed_pairs;
  failed_pairs.reserve(failed_vars.size());
  for (size_t i = 0; i < failed_vars.size(); ++i) {
    failed_pairs.emplace_back(failed_vars[i], failed_values[i]);
  }

  std::sort(failed_pairs.begin(), failed_pairs.end());
  return failed_pairs;
}

std::vector<std::pair<int, int>> RunBatch2MicroBatch(
    GModel* gmodel,
    const std::vector<ProbeTask>& tasks,
    int activation_strategy,
    int micro_batch_size,
    bool enable_precheck) {
  Batch2ProbeManager batch2_mgr(gmodel, micro_batch_size);
  batch2_mgr.SetActivationStrategy(activation_strategy);
  batch2_mgr.EnablePrecheck(enable_precheck);

  for (const auto& t : tasks) {
    batch2_mgr.AddTask(t.var_id, t.value);
  }

  std::vector<int> failed_vars;
  std::vector<int> failed_values;
  batch2_mgr.ExecuteMicroBatch(failed_vars, failed_values);

  std::vector<std::pair<int, int>> failed_pairs;
  failed_pairs.reserve(failed_vars.size());
  for (size_t i = 0; i < failed_vars.size(); ++i) {
    failed_pairs.emplace_back(failed_vars[i], failed_values[i]);
  }

  std::sort(failed_pairs.begin(), failed_pairs.end());
  return failed_pairs;
}

bool TestSingleStrategy(GModel* gmodel, int strategy) {
  const int num_vars = gmodel->GetNumVars();
  const int bit_dom_int_size = gmodel->GetBitDomIntSize();
  const int snapshot_size_words = num_vars * bit_dom_int_size;

  const char* strategy_name =
      (strategy == 0) ? "FULL_ACTIVATION" : "NEIGHBOR_ACTIVATION";

  LOG(INFO) << "----------------------------------------";
  LOG(INFO) << "Strategy: " << strategy_name;

  const auto baseline_bitDom =
      CopyFromDevice(gmodel->GetBitDom(), snapshot_size_words);
  const auto baseline_dom_sizes =
      CopyFromDevice(gmodel->GetDomainSizesPtr(), num_vars);

  const auto tasks = CollectAllProbeTasks(gmodel);
  LOG(INFO) << "Collected probe tasks: " << tasks.size();

  // Batch-1 结果（cooperative，时间维度 batching）
  const auto failed_batch1 = RunBatch1(gmodel, tasks, strategy);

  // Batch-1 执行后状态不应变化
  const auto after_batch1_bitDom =
      CopyFromDevice(gmodel->GetBitDom(), snapshot_size_words);
  const auto after_batch1_dom_sizes =
      CopyFromDevice(gmodel->GetDomainSizesPtr(), num_vars);

  bool batch1_state_ok =
      ArraysEqual(after_batch1_bitDom.data(), baseline_bitDom.data(),
                  snapshot_size_words, "bitDom(after Batch-1)") &&
      ArraysEqual(after_batch1_dom_sizes.data(), baseline_dom_sizes.data(),
                  num_vars, "d_cur_dom_size(after Batch-1)");
  CHECK(batch1_state_ok) << "Batch-1 polluted GModel state (unexpected)";

  // Batch-2 Micro-Batch 结果（非 cooperative，空间并行）
  const int micro_batch_size = 4;  // 故意设小，覆盖“分批”路径
  {
    const bool enable_precheck = false;
    const auto failed_batch2 =
        RunBatch2MicroBatch(gmodel, tasks, strategy, micro_batch_size,
                            enable_precheck);

    // Batch-2 执行后状态不应变化
    const auto after_batch2_bitDom =
        CopyFromDevice(gmodel->GetBitDom(), snapshot_size_words);
    const auto after_batch2_dom_sizes =
        CopyFromDevice(gmodel->GetDomainSizesPtr(), num_vars);

    bool batch2_state_ok =
        ArraysEqual(after_batch2_bitDom.data(), baseline_bitDom.data(),
                    snapshot_size_words, "bitDom(after Batch-2)") &&
        ArraysEqual(after_batch2_dom_sizes.data(), baseline_dom_sizes.data(),
                    num_vars, "d_cur_dom_size(after Batch-2)");

    if (!batch2_state_ok) {
      LOG(ERROR) << "Batch-2 Micro-Batch state consistency: FAIL";
      return false;
    }
    LOG(INFO) << "Batch-2 Micro-Batch state consistency: PASS";

    // 结果一致性
    if (failed_batch1 != failed_batch2) {
      LOG(ERROR) << "Mismatch between Batch-1 and Batch-2 results";
      LOG(ERROR) << "  Batch-1 failures: " << failed_batch1.size();
      LOG(ERROR) << "  Batch-2 failures: " << failed_batch2.size();

      const size_t n = std::min(failed_batch1.size(), failed_batch2.size());
      for (size_t i = 0; i < n; ++i) {
        if (failed_batch1[i] != failed_batch2[i]) {
          LOG(ERROR) << "First diff at " << i
                     << ": Batch-1=(" << failed_batch1[i].first << ","
                     << failed_batch1[i].second << ")"
                     << " vs Batch-2=(" << failed_batch2[i].first << ","
                     << failed_batch2[i].second << ")";
          break;
        }
      }
      return false;
    }

    LOG(INFO) << "Batch-1 vs Batch-2 result consistency: PASS ("
              << failed_batch2.size() << " failed probes)";
  }

  // 再跑一次：Batch-2 开启 precheck（在 AC snapshot 前提下预期不影响结果）
  {
    const bool enable_precheck = true;
    const auto failed_batch2 =
        RunBatch2MicroBatch(gmodel, tasks, strategy, micro_batch_size,
                            enable_precheck);

    const auto after_batch2_bitDom =
        CopyFromDevice(gmodel->GetBitDom(), snapshot_size_words);
    const auto after_batch2_dom_sizes =
        CopyFromDevice(gmodel->GetDomainSizesPtr(), num_vars);

    bool batch2_state_ok =
        ArraysEqual(after_batch2_bitDom.data(), baseline_bitDom.data(),
                    snapshot_size_words, "bitDom(after Batch-2 + precheck)") &&
        ArraysEqual(after_batch2_dom_sizes.data(), baseline_dom_sizes.data(),
                    num_vars, "d_cur_dom_size(after Batch-2 + precheck)");
    if (!batch2_state_ok) {
      LOG(ERROR) << "Batch-2 Micro-Batch (+precheck) state consistency: FAIL";
      return false;
    }
    LOG(INFO) << "Batch-2 Micro-Batch (+precheck) state consistency: PASS";

    if (failed_batch1 != failed_batch2) {
      LOG(ERROR) << "Mismatch between Batch-1 and Batch-2 (+precheck) results";
      LOG(ERROR) << "  Batch-1 failures: " << failed_batch1.size();
      LOG(ERROR) << "  Batch-2 failures: " << failed_batch2.size();
      return false;
    }
    LOG(INFO) << "Batch-1 vs Batch-2 (+precheck) result consistency: PASS";
  }

  return true;
}

bool TestBatch2MicroBatch() {
  LOG(INFO) << "========================================";
  LOG(INFO) << "Test: Batch-2 Micro-Batch vs Batch-1";
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

  LOG(INFO) << "Running initial GAC propagation (snapshot baseline)...";
  auto gac_stats = gmodel->EnforceGAC_Persistent(false, -1);
  if (gac_stats.inconsistent) {
    LOG(INFO) << "Initial GAC detected inconsistency (UNSAT instance)";
    LOG(INFO) << "✓ Test skipped (UNSAT instance)";
    return true;
  }

  LOG(INFO) << "Initial GAC: " << gac_stats.deletions << " deletions, "
            << gac_stats.iterations << " iterations";

  bool full_ok = TestSingleStrategy(gmodel, 0);
  bool neighbor_ok = TestSingleStrategy(gmodel, 1);

  LOG(INFO) << "========================================";
  LOG(INFO) << "Final Results:";
  LOG(INFO) << "  FULL_ACTIVATION:     " << (full_ok ? "✓ PASS" : "✗ FAIL");
  LOG(INFO) << "  NEIGHBOR_ACTIVATION: " << (neighbor_ok ? "✓ PASS" : "✗ FAIL");
  LOG(INFO) << "========================================";

  return full_ok && neighbor_ok;
}

}  // namespace cpim

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  FLAGS_logtostderr = 1;
  FLAGS_v = 1;

  return cpim::TestBatch2MicroBatch() ? 0 : 1;
}
