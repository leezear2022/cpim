// Copyright 2026 CPIM Project
// FQ-PT baseline correctness smoke test

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
DEFINE_int32(num_probes, 16, "Number of probe tasks to run");
DEFINE_int32(fqpt_queue_capacity, 0, "FQ-PT queue capacity (power of two, 0=auto)");
DEFINE_int32(fqpt_pop_batch, 4, "FQ-PT CTA batch pop size");
DEFINE_int32(fqpt_local_buffer, 64, "FQ-PT CTA local buffer size");
DEFINE_bool(fqpt_enable_world_owner, false,
            "Enable Owner-World + two-level frontier path");
DEFINE_bool(fqpt_enable_world_stealing, false,
            "Enable OW2 world_cursor dynamic world assignment control path");
DEFINE_bool(fqpt_enable_ow1_frontier_scatter, false,
            "Enable OW1 warp-cooperative frontier scatter");
DEFINE_int32(fqpt_ow1_min_degree, 32,
             "OW1 min variable degree to trigger warp scatter");
DEFINE_int32(fqpt_ow1_scatter_mode, 1,
             "OW1 scatter mode (0=fallback, 1=legacy warp, 2=match_any)");
DEFINE_bool(fqpt_ow1_force_scatter, false,
            "OW1 force scatter regardless of min degree");
DEFINE_bool(fqpt_enable_cid_microbatch, false,
            "Enable OW3b cid micro-batch framework path");
DEFINE_int32(fqpt_microbatch_min_sel, 2,
             "OW3b minimum sel_count for aligned execution");
DEFINE_int32(fqpt_microbatch_warps, 8,
             "OW3b max warps participating in micro-batch (1..8)");
DEFINE_int32(fqpt_microbatch_max_rounds, 0,
             "OW3b max per-world micro-batch rounds (0=unlimited)");
DEFINE_bool(fqpt_enable_cid_microbatch_profile, false,
            "Enable OW3a cid micro-batch profile counters");
DEFINE_int32(fqpt_microbatch_profile_interval, 64,
             "OW3a profile sampling interval in propagation rounds");

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
      tasks.emplace_back(var, val, static_cast<int>(tasks.size()));
    }
  }
  return tasks;
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

  std::vector<std::pair<int, int>> failed;
  failed.reserve(failed_vars.size());
  for (size_t i = 0; i < failed_vars.size(); ++i) {
    failed.emplace_back(failed_vars[i], failed_values[i]);
  }
  std::sort(failed.begin(), failed.end());
  return failed;
}

void RunFQPT(
    GModel* gmodel,
    const std::vector<ProbeTask>& tasks,
    std::vector<std::pair<int, int>>* failed,
    std::vector<std::pair<int, int>>* unknown,
    FQPTStatistics* stats) {
  FQPTBaselineManager mgr(gmodel, -1);
  if (FLAGS_fqpt_queue_capacity > 0) {
    mgr.SetQueueCapacity(FLAGS_fqpt_queue_capacity);
  }
  mgr.SetCtaPopBatch(FLAGS_fqpt_pop_batch);
  mgr.SetLocalBufferCapacity(FLAGS_fqpt_local_buffer);
  mgr.SetEnableWorldOwner(FLAGS_fqpt_enable_world_owner);
  mgr.SetEnableWorldStealing(FLAGS_fqpt_enable_world_stealing);
  mgr.SetEnableOW1FrontierScatter(FLAGS_fqpt_enable_ow1_frontier_scatter);
  mgr.SetOW1MinDegree(FLAGS_fqpt_ow1_min_degree);
  mgr.SetOW1ScatterMode(FLAGS_fqpt_ow1_scatter_mode);
  mgr.SetOW1ForceScatter(FLAGS_fqpt_ow1_force_scatter);
  mgr.SetEnableCidMicrobatch(FLAGS_fqpt_enable_cid_microbatch);
  mgr.SetMicrobatchMinSel(FLAGS_fqpt_microbatch_min_sel);
  mgr.SetMicrobatchWarps(FLAGS_fqpt_microbatch_warps);
  mgr.SetMicrobatchMaxRounds(FLAGS_fqpt_microbatch_max_rounds);
  mgr.SetEnableCidMicrobatchProfile(FLAGS_fqpt_enable_cid_microbatch_profile);
  mgr.SetMicrobatchProfileInterval(FLAGS_fqpt_microbatch_profile_interval);

  for (const auto& t : tasks) {
    mgr.AddTask(t.var_id, t.value);
  }

  std::vector<int> failed_vars, failed_values;
  std::vector<int> unknown_vars, unknown_values;
  mgr.Execute(failed_vars, failed_values, &unknown_vars, &unknown_values);

  failed->clear();
  failed->reserve(failed_vars.size());
  for (size_t i = 0; i < failed_vars.size(); ++i) {
    failed->emplace_back(failed_vars[i], failed_values[i]);
  }
  std::sort(failed->begin(), failed->end());

  unknown->clear();
  unknown->reserve(unknown_vars.size());
  for (size_t i = 0; i < unknown_vars.size(); ++i) {
    unknown->emplace_back(unknown_vars[i], unknown_values[i]);
  }
  std::sort(unknown->begin(), unknown->end());

  *stats = mgr.GetLastStatistics();
}

bool TestFQPTBaseline() {
  LOG(INFO) << "========================================";
  LOG(INFO) << "Test: FQ-PT Baseline";
  LOG(INFO) << "========================================";

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

  auto gmodel_obj = model::GModelAdapter::Build(*normalized_or);
  GModel* gmodel = &gmodel_obj;

  auto gac_stats = gmodel->EnforceGAC_Persistent(false, -1);
  if (gac_stats.inconsistent) {
    LOG(INFO) << "Initial GAC detected inconsistency, skip";
    return true;
  }

  auto tasks = CollectProbeTasks(gmodel, FLAGS_num_probes);
  if (tasks.empty()) {
    LOG(INFO) << "No probe tasks, skip";
    return true;
  }

  auto t0 = std::chrono::high_resolution_clock::now();
  auto failed_stage2 = RunStage2(gmodel, tasks);
  auto t1 = std::chrono::high_resolution_clock::now();
  const double stage2_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();

  std::vector<std::pair<int, int>> failed_fqpt;
  std::vector<std::pair<int, int>> unknown_fqpt;
  FQPTStatistics fqpt_stats;

  auto t2 = std::chrono::high_resolution_clock::now();
  RunFQPT(gmodel, tasks, &failed_fqpt, &unknown_fqpt, &fqpt_stats);
  auto t3 = std::chrono::high_resolution_clock::now();
  const double fqpt_ms =
      std::chrono::duration<double, std::milli>(t3 - t2).count();

  LOG(INFO) << "Stage2: failed=" << failed_stage2.size() << ", time="
            << stage2_ms << "ms";
  LOG(INFO) << "FQPT: failed=" << failed_fqpt.size()
            << ", unknown=" << unknown_fqpt.size()
            << ", time=" << fqpt_ms << "ms";
  LOG(INFO) << "FQPT stats: processed_tasks=" << fqpt_stats.processed_tasks
            << ", overflow=" << fqpt_stats.overflow_count
            << ", checks=" << fqpt_stats.constraint_checks
            << ", deletions=" << fqpt_stats.deletions;

  std::set<std::pair<int, int>> stage2_set(failed_stage2.begin(), failed_stage2.end());

  if (unknown_fqpt.empty()) {
    if (failed_fqpt != failed_stage2) {
      LOG(ERROR) << "FQPT mismatch without UNKNOWN";
      return false;
    }
  } else {
    for (const auto& p : failed_fqpt) {
      if (stage2_set.find(p) == stage2_set.end()) {
        LOG(ERROR) << "FQPT produced unsafe DWO under UNKNOWN: var="
                   << p.first << ", val=" << p.second;
        return false;
      }
    }
  }

  LOG(INFO) << "FQ-PT baseline test passed";
  return true;
}

}  // namespace cpim

int main(int argc, char* argv[]) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  const bool ok = cpim::TestFQPTBaseline();
  return ok ? 0 : 1;
}
