// benchmark_probe_throughput.cpp
// Batch-1 vs Batch-2 Stage 1 vs Stage 2 吞吐量基准测试
//
// 用法:
//   ./benchmark_probe_throughput <instance.xml> [options]
//
// 选项:
//   --runs=N          运行次数（默认 3）
//   --batch2_size=M   Stage 1 Micro-Batch 大小（默认 64）
//   --stage2_blocks=B Stage 2 Persistent Blocks 数量（-1=自动，默认 -1）
//   --strategy=S      激活策略：0=FULL, 1=NEIGHBOR（默认 1）
//   --precheck        启用 precheck
//
// 输出:
//   - 总 probe 数
//   - Batch-1 / Stage 1 / Stage 2 执行时间和吞吐量 (probes/s)
//   - Stage 1 vs Stage 2 加速比
//   - 统计信息：avg_iterations, avg_deletions, fail_rate

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <numeric>

#include "GModel.cuh"
#include "model/gmodel_adapter.h"
#include "model/intermediate_model.h"
#include "model/model_normalizer.h"
#include "model/xcsp_parser.h"
#include "solver/gpu/batch_probe_manager.h"

using namespace cpim;
using namespace cpim::model;

// 收集所有 probe 任务（变量-值对）
std::vector<ProbeTask> CollectAllProbeTasks(GModel* gmodel) {
  std::vector<ProbeTask> tasks;
  const int num_vars = gmodel->GetNumVars();

  for (int var = 0; var < num_vars; ++var) {
    if (gmodel->IsAssigned(var)) continue;

    for (int val = gmodel->GetFirstValue(var); val != -1;
         val = gmodel->GetNextValue(var, val)) {
      tasks.emplace_back(ProbeTask{var, val, static_cast<int>(tasks.size())});
    }
  }
  return tasks;
}

// 运行 Batch-1 并返回执行时间（秒）
double RunBatch1(GModel* gmodel, const std::vector<ProbeTask>& tasks,
                 int activation_strategy, int& num_failed) {
  BatchProbeManager batch1(gmodel, static_cast<int>(tasks.size()));
  batch1.SetActivationStrategy(activation_strategy);

  for (const auto& t : tasks) {
    batch1.AddTask(t.var_id, t.value);
  }

  std::vector<int> failed_vars, failed_values;

  auto start = std::chrono::high_resolution_clock::now();
  batch1.ExecuteBatch(failed_vars, failed_values);
  auto end = std::chrono::high_resolution_clock::now();

  num_failed = static_cast<int>(failed_vars.size());
  return std::chrono::duration<double>(end - start).count();
}

// 运行 Batch-2 Stage 1 (Micro-Batch) 并返回执行时间（秒）
double RunStage1MicroBatch(GModel* gmodel, const std::vector<ProbeTask>& tasks,
                           int activation_strategy, int micro_batch_size,
                           bool enable_precheck, int& num_failed,
                           double& avg_iterations, double& avg_deletions) {
  Batch2ProbeManager batch2(gmodel, micro_batch_size);
  batch2.SetActivationStrategy(activation_strategy);
  batch2.EnablePrecheck(enable_precheck);
  batch2.EnableStats(true);

  for (const auto& t : tasks) {
    batch2.AddTask(t.var_id, t.value);
  }

  std::vector<int> failed_vars, failed_values;

  auto start = std::chrono::high_resolution_clock::now();
  batch2.ExecuteMicroBatch(failed_vars, failed_values);
  auto end = std::chrono::high_resolution_clock::now();

  num_failed = static_cast<int>(failed_vars.size());

  // 计算平均迭代数和删值数
  const auto& iterations = batch2.GetLastProbeIterations();
  const auto& deletions = batch2.GetLastProbeDeletions();

  double total_iter = 0, total_del = 0;
  for (size_t i = 0; i < iterations.size(); ++i) {
    total_iter += iterations[i];
    total_del += deletions[i];
  }
  avg_iterations = iterations.empty() ? 0 : total_iter / iterations.size();
  avg_deletions = deletions.empty() ? 0 : total_del / deletions.size();

  return std::chrono::duration<double>(end - start).count();
}

// 运行 Batch-2 Stage 2 (Persistent Blocks) 并返回执行时间（秒）
double RunStage2PersistentBlocks(GModel* gmodel, const std::vector<ProbeTask>& tasks,
                                 int activation_strategy, int num_blocks,
                                 bool enable_precheck, int& num_failed,
                                 double& avg_iterations, double& avg_deletions,
                                 int& effective_blocks) {
  Batch2PersistentManager stage2(gmodel, num_blocks);
  stage2.SetActivationStrategy(activation_strategy);
  stage2.EnablePrecheck(enable_precheck);
  stage2.EnableStats(true);

  for (const auto& t : tasks) {
    stage2.AddTask(t.var_id, t.value);
  }

  std::vector<int> failed_vars, failed_values;

  auto start = std::chrono::high_resolution_clock::now();
  stage2.ExecutePersistentBlocks(failed_vars, failed_values);
  auto end = std::chrono::high_resolution_clock::now();

  num_failed = static_cast<int>(failed_vars.size());
  effective_blocks = stage2.GetEffectiveNumBlocks();

  // 计算平均迭代数和删值数
  const auto& iterations = stage2.GetLastProbeIterations();
  const auto& deletions = stage2.GetLastProbeDeletions();

  double total_iter = 0, total_del = 0;
  for (size_t i = 0; i < iterations.size(); ++i) {
    total_iter += iterations[i];
    total_del += deletions[i];
  }
  avg_iterations = iterations.empty() ? 0 : total_iter / iterations.size();
  avg_deletions = deletions.empty() ? 0 : total_del / deletions.size();

  return std::chrono::duration<double>(end - start).count();
}

// 使用 AutoStageSelector 进行自动选择（基于统计）
void RunAutoSelection(GModel* gmodel, const std::vector<ProbeTask>& tasks,
                      int activation_strategy, bool enable_precheck,
                      int batch_size) {
  std::cout << "\n[Auto Stage Selection (Statistics-based)]\n";

  AutoStageSelector selector(gmodel, 16);  // 采样 16 个 probes
  auto result = selector.DecideWithSampling(tasks);

  std::cout << "Decision: " << (result.stage == StageSelection::kStage1 ? "Stage 1" : "Stage 2") << "\n";
  std::cout << "Reason: " << result.reason << "\n";

  if (result.sampled) {
    std::cout << "Sampling stats:\n";
    std::cout << "  fail_rate: " << std::fixed << std::setprecision(1)
              << (result.sample_fail_rate * 100) << "%\n";
    std::cout << "  avg_iterations: " << result.sample_avg_iterations << "\n";
    std::cout << "  avg_deletions: " << result.sample_avg_deletions << "\n";
  }

  // 运行选中的 stage
  std::cout << "\nRunning selected stage...\n";

  int num_failed = 0;
  double elapsed = 0;
  double avg_iter = 0, avg_del = 0;

  if (result.stage == StageSelection::kStage1) {
    elapsed = RunStage1MicroBatch(gmodel, tasks, activation_strategy,
                                   batch_size, enable_precheck, num_failed,
                                   avg_iter, avg_del);
    std::cout << "Stage 1 result:\n";
  } else {
    int eff_blocks = 0;
    elapsed = RunStage2PersistentBlocks(gmodel, tasks, activation_strategy,
                                         result.recommended_blocks, enable_precheck,
                                         num_failed, avg_iter, avg_del, eff_blocks);
    std::cout << "Stage 2 result (blocks=" << eff_blocks << "):\n";
  }

  std::cout << "  Time: " << std::fixed << std::setprecision(3) << (elapsed * 1000) << " ms\n";
  std::cout << "  Throughput: " << std::setprecision(0)
            << (tasks.size() / elapsed) << " probes/s\n";
  std::cout << "  Failed: " << num_failed << " / " << tasks.size() << "\n";
}

// 使用 AutoStageSelector 进行自动选择（基于实测对比，推荐）
void RunAutoSelectionTimed(GModel* gmodel, const std::vector<ProbeTask>& tasks,
                           int activation_strategy, bool enable_precheck,
                           int batch_size, int stage2_blocks) {
  std::cout << "\n[Auto Stage Selection (Timed Comparison)]\n";

  // 采样大小：取任务数的 10% 和 64 中的较小值，但至少 16
  int sample_size = std::max(16, std::min(64, static_cast<int>(tasks.size()) / 10));
  AutoStageSelector selector(gmodel, sample_size);
  auto result = selector.DecideWithTimedComparison(tasks, stage2_blocks);

  std::cout << "Decision: " << (result.stage == StageSelection::kStage1 ? "Stage 1" : "Stage 2") << "\n";
  std::cout << "Reason: " << result.reason << "\n";

  if (result.timed) {
    std::cout << "Timed comparison:\n";
    std::cout << "  Stage 1: " << std::fixed << std::setprecision(2)
              << result.stage1_time_ms << " ms\n";
    std::cout << "  Stage 2: " << result.stage2_time_ms << " ms\n";
    std::cout << "  Speedup: " << result.speedup_ratio << "x\n";
  }

  if (result.sampled) {
    std::cout << "Sample stats:\n";
    std::cout << "  fail_rate: " << std::fixed << std::setprecision(1)
              << (result.sample_fail_rate * 100) << "%\n";
    std::cout << "  avg_iterations: " << result.sample_avg_iterations << "\n";
    std::cout << "  avg_deletions: " << result.sample_avg_deletions << "\n";
  }

  // 运行选中的 stage
  std::cout << "\nRunning selected stage on full workload...\n";

  int num_failed = 0;
  double elapsed = 0;
  double avg_iter = 0, avg_del = 0;

  if (result.stage == StageSelection::kStage1) {
    elapsed = RunStage1MicroBatch(gmodel, tasks, activation_strategy,
                                   batch_size, enable_precheck, num_failed,
                                   avg_iter, avg_del);
    std::cout << "Stage 1 result:\n";
  } else {
    int eff_blocks = 0;
    elapsed = RunStage2PersistentBlocks(gmodel, tasks, activation_strategy,
                                         result.recommended_blocks, enable_precheck,
                                         num_failed, avg_iter, avg_del, eff_blocks);
    std::cout << "Stage 2 result (blocks=" << eff_blocks << "):\n";
  }

  std::cout << "  Time: " << std::fixed << std::setprecision(3) << (elapsed * 1000) << " ms\n";
  std::cout << "  Throughput: " << std::setprecision(0)
            << (tasks.size() / elapsed) << " probes/s\n";
  std::cout << "  Failed: " << num_failed << " / " << tasks.size() << "\n";
}

// 运行带缓存的自动选择（演示缓存行为）
void RunAutoSelectionCached(GModel* gmodel, const std::vector<ProbeTask>& tasks,
                            int activation_strategy, bool enable_precheck,
                            int batch_size, int stage2_blocks) {
  std::cout << "\n[Auto Stage Selection (Cached - Production Mode)]\n";

  // 采样大小：取任务数的 10% 和 64 中的较小值，但至少 16
  int sample_size = std::max(16, std::min(64, static_cast<int>(tasks.size()) / 10));
  AutoStageSelector selector(gmodel, sample_size);

  // 第一次调用：执行采样
  std::cout << "=== First call (will sample) ===\n";
  auto result1 = selector.DecideCached(tasks, stage2_blocks);
  std::cout << "Decision: " << (result1.stage == StageSelection::kStage1 ? "Stage 1" : "Stage 2") << "\n";
  std::cout << "HasCache: " << (selector.HasCache() ? "true" : "false") << "\n\n";

  // 第二次调用：使用缓存
  std::cout << "=== Second call (will use cache) ===\n";
  auto result2 = selector.DecideCached(tasks, stage2_blocks);
  std::cout << "Decision: " << (result2.stage == StageSelection::kStage1 ? "Stage 1" : "Stage 2") << "\n";
  std::cout << "HasCache: " << (selector.HasCache() ? "true" : "false") << "\n\n";

  // 第三次调用：清除缓存后重新采样
  std::cout << "=== Third call (after ClearCache) ===\n";
  selector.ClearCache();
  std::cout << "HasCache after ClearCache: " << (selector.HasCache() ? "true" : "false") << "\n";
  auto result3 = selector.DecideCached(tasks, stage2_blocks);
  std::cout << "Decision: " << (result3.stage == StageSelection::kStage1 ? "Stage 1" : "Stage 2") << "\n";
  std::cout << "HasCache: " << (selector.HasCache() ? "true" : "false") << "\n\n";

  // 运行选中的 stage
  std::cout << "=== Running selected stage on full workload ===\n";

  int num_failed = 0;
  double elapsed = 0;
  double avg_iter = 0, avg_del = 0;

  if (result1.stage == StageSelection::kStage1) {
    elapsed = RunStage1MicroBatch(gmodel, tasks, activation_strategy,
                                   batch_size, enable_precheck, num_failed,
                                   avg_iter, avg_del);
    std::cout << "Stage 1 result:\n";
  } else {
    int eff_blocks = 0;
    elapsed = RunStage2PersistentBlocks(gmodel, tasks, activation_strategy,
                                         result1.recommended_blocks, enable_precheck,
                                         num_failed, avg_iter, avg_del, eff_blocks);
    std::cout << "Stage 2 result (blocks=" << eff_blocks << "):\n";
  }

  std::cout << "  Time: " << std::fixed << std::setprecision(3) << (elapsed * 1000) << " ms\n";
  std::cout << "  Throughput: " << std::setprecision(0)
            << (tasks.size() / elapsed) << " probes/s\n";
  std::cout << "  Failed: " << num_failed << " / " << tasks.size() << "\n";
}

void PrintUsage(const char* prog) {
  std::cout << "Usage: " << prog << " <instance.xml> [options]\n\n";
  std::cout << "Options:\n";
  std::cout << "  --runs=N          : Number of runs for averaging (default: 3)\n";
  std::cout << "  --batch2_size=M   : Stage 1 micro-batch size (default: 64)\n";
  std::cout << "  --stage2_blocks=B : Stage 2 persistent blocks (-1=auto, default: -1)\n";
  std::cout << "  --strategy=S      : Activation strategy: 0=FULL, 1=NEIGHBOR (default: 1)\n";
  std::cout << "  --precheck        : Enable precheck (default: disabled)\n";
  std::cout << "  --skip-batch1     : Skip Batch-1 (faster, focus on Stage 1 vs Stage 2)\n";
  std::cout << "  --auto            : Use AutoStageSelector (statistics-based)\n";
  std::cout << "  --auto-timed      : Use AutoStageSelector (timed comparison, recommended)\n";
  std::cout << "  --auto-cached     : Demo DecideCached (for production use in SAC/MSAC)\n";
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    PrintUsage(argv[0]);
    return 1;
  }

  std::string input_path = argv[1];
  int num_runs = 3;
  int batch2_size = 64;
  int stage2_blocks = -1;  // -1 = auto
  int activation_strategy = 1;  // NEIGHBOR_ACTIVATION
  bool enable_precheck = false;
  bool skip_batch1 = false;
  bool auto_mode = false;
  bool auto_timed_mode = false;
  bool auto_cached_mode = false;

  // 解析参数
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--runs=", 0) == 0) {
      num_runs = std::stoi(arg.substr(7));
    } else if (arg.rfind("--batch2_size=", 0) == 0) {
      batch2_size = std::stoi(arg.substr(14));
    } else if (arg.rfind("--stage2_blocks=", 0) == 0) {
      stage2_blocks = std::stoi(arg.substr(16));
    } else if (arg.rfind("--strategy=", 0) == 0) {
      activation_strategy = std::stoi(arg.substr(11));
    } else if (arg == "--precheck") {
      enable_precheck = true;
    } else if (arg == "--skip-batch1") {
      skip_batch1 = true;
    } else if (arg == "--auto-cached") {
      auto_cached_mode = true;
    } else if (arg == "--auto-timed") {
      auto_timed_mode = true;
    } else if (arg == "--auto") {
      auto_mode = true;
    }
  }

  std::cout << "================================================\n";
  std::cout << "Stage 1 vs Stage 2 Throughput Benchmark\n";
  std::cout << "================================================\n";
  std::cout << "Instance: " << input_path << "\n";
  std::cout << "Runs: " << num_runs << "\n";
  std::cout << "Stage 1 micro_batch_size: " << batch2_size << "\n";
  std::cout << "Stage 2 blocks: " << (stage2_blocks == -1 ? "auto" : std::to_string(stage2_blocks)) << "\n";
  std::cout << "Activation strategy: "
            << (activation_strategy == 0 ? "FULL" : "NEIGHBOR") << "\n";
  std::cout << "Precheck: " << (enable_precheck ? "enabled" : "disabled") << "\n";
  std::cout << "------------------------------------------------\n";

  // 1. 加载并解析问题
  std::cout << "[1/4] Loading instance..." << std::flush;
  auto parser = XcspParser::Create(ParserType::kLibXml2);
  auto model_or = parser->Parse(input_path);
  if (!model_or.ok()) {
    std::cerr << "\nFailed to parse: " << model_or.status() << "\n";
    return 1;
  }

  ModelNormalizer normalizer;
  auto normalized_or = normalizer.Normalize(*model_or);
  if (!normalized_or.ok()) {
    std::cerr << "\nFailed to normalize: " << normalized_or.status() << "\n";
    return 1;
  }
  const IntermediateModel& im_model = *normalized_or;
  std::cout << " OK\n";

  // 2. 构建 GModel
  std::cout << "[2/4] Building GModel..." << std::flush;
  GModelOptions options;
  options.device_id = 0;
  GModel gmodel = GModelAdapter::Build(im_model, options);
  std::cout << " OK\n";

  // 3. 运行初始 GAC
  std::cout << "[3/4] Running initial GAC..." << std::flush;
  auto gac_stats = gmodel.EnforceGAC_Persistent(false, -1);
  std::cout << " OK (deletions=" << gac_stats.deletions << ")\n";

  // 4. 收集所有 probe 任务
  std::cout << "[4/4] Collecting probe tasks..." << std::flush;
  auto tasks = CollectAllProbeTasks(&gmodel);
  const int total_probes = static_cast<int>(tasks.size());
  std::cout << " OK (" << total_probes << " probes)\n";

  if (total_probes == 0) {
    std::cout << "\nNo probes to run (all variables assigned or empty domains)\n";
    return 0;
  }

  // 自动模式（带缓存，生产用法演示）
  if (auto_cached_mode) {
    RunAutoSelectionCached(&gmodel, tasks, activation_strategy, enable_precheck,
                           batch2_size, stage2_blocks);
    return 0;
  }

  // 自动模式（实测对比，推荐）
  if (auto_timed_mode) {
    RunAutoSelectionTimed(&gmodel, tasks, activation_strategy, enable_precheck,
                          batch2_size, stage2_blocks);
    return 0;
  }

  // 自动模式（统计）
  if (auto_mode) {
    RunAutoSelection(&gmodel, tasks, activation_strategy, enable_precheck, batch2_size);
    return 0;
  }

  std::cout << "------------------------------------------------\n";
  std::cout << "Running " << num_runs << " iterations...\n\n";

  // 运行多次并收集统计
  std::vector<double> batch1_times, stage1_times, stage2_times;
  int batch1_failed = 0, stage1_failed = 0, stage2_failed = 0;
  double stage1_avg_iter = 0, stage1_avg_del = 0;
  double stage2_avg_iter = 0, stage2_avg_del = 0;
  int stage2_effective_blocks = 0;

  for (int run = 0; run < num_runs; ++run) {
    std::cout << "Run " << (run + 1) << "/" << num_runs << "..." << std::flush;

    // Batch-1 (optional)
    if (!skip_batch1) {
      int failed1 = 0;
      double t1 = RunBatch1(&gmodel, tasks, activation_strategy, failed1);
      batch1_times.push_back(t1);
      batch1_failed = failed1;
    }

    // Stage 1 (Micro-Batch)
    int failed_s1 = 0;
    double iter1 = 0, del1 = 0;
    double t_s1 = RunStage1MicroBatch(&gmodel, tasks, activation_strategy,
                                       batch2_size, enable_precheck, failed_s1,
                                       iter1, del1);
    stage1_times.push_back(t_s1);
    stage1_failed = failed_s1;
    stage1_avg_iter = iter1;
    stage1_avg_del = del1;

    // Stage 2 (Persistent Blocks)
    int failed_s2 = 0;
    double iter2 = 0, del2 = 0;
    int eff_blocks = 0;
    double t_s2 = RunStage2PersistentBlocks(&gmodel, tasks, activation_strategy,
                                             stage2_blocks, enable_precheck, failed_s2,
                                             iter2, del2, eff_blocks);
    stage2_times.push_back(t_s2);
    stage2_failed = failed_s2;
    stage2_avg_iter = iter2;
    stage2_avg_del = del2;
    stage2_effective_blocks = eff_blocks;

    std::cout << std::fixed << std::setprecision(3);
    if (!skip_batch1) {
      std::cout << " B1=" << batch1_times.back() * 1000 << "ms,";
    }
    std::cout << " S1=" << t_s1 * 1000 << "ms, S2=" << t_s2 * 1000 << "ms";
    std::cout << " (blocks=" << eff_blocks << ")\n";
  }

  // 计算平均值
  double batch1_avg = 0;
  if (!skip_batch1 && !batch1_times.empty()) {
    batch1_avg = std::accumulate(batch1_times.begin(), batch1_times.end(), 0.0) / num_runs;
  }
  double stage1_avg =
      std::accumulate(stage1_times.begin(), stage1_times.end(), 0.0) / num_runs;
  double stage2_avg =
      std::accumulate(stage2_times.begin(), stage2_times.end(), 0.0) / num_runs;

  double batch1_throughput = (batch1_avg > 0) ? (total_probes / batch1_avg) : 0;
  double stage1_throughput = total_probes / stage1_avg;
  double stage2_throughput = total_probes / stage2_avg;
  double stage2_vs_stage1 = stage1_avg / stage2_avg;

  double fail_rate = static_cast<double>(stage1_failed) / total_probes * 100.0;

  // 输出结果
  std::cout << "\n================================================\n";
  std::cout << "RESULTS\n";
  std::cout << "================================================\n";
  std::cout << "Total probes: " << total_probes << "\n";
  std::cout << "Failed probes: " << stage1_failed
            << " (fail_rate=" << std::fixed << std::setprecision(1) << fail_rate << "%)\n";
  std::cout << "Consistency check (S1 vs S2): "
            << (stage1_failed == stage2_failed ? "PASS" : "FAIL") << "\n";
  std::cout << "------------------------------------------------\n";

  std::cout << std::fixed << std::setprecision(3);

  if (!skip_batch1 && batch1_avg > 0) {
    std::cout << "Batch-1 (baseline):\n";
    std::cout << "  Avg time: " << batch1_avg * 1000 << " ms\n";
    std::cout << "  Throughput: " << std::setprecision(0) << batch1_throughput << " probes/s\n";
    std::cout << std::setprecision(3);
  }

  std::cout << "Stage 1 (Micro-Batch, batch_size=" << batch2_size << "):\n";
  std::cout << "  Avg time: " << stage1_avg * 1000 << " ms\n";
  std::cout << "  Throughput: " << std::setprecision(0) << stage1_throughput << " probes/s\n";
  std::cout << std::setprecision(1);
  std::cout << "  Avg iterations/probe: " << stage1_avg_iter << "\n";
  std::cout << "  Avg deletions/probe: " << stage1_avg_del << "\n";

  std::cout << std::setprecision(3);
  std::cout << "Stage 2 (Persistent Blocks, blocks=" << stage2_effective_blocks << "):\n";
  std::cout << "  Avg time: " << stage2_avg * 1000 << " ms\n";
  std::cout << "  Throughput: " << std::setprecision(0) << stage2_throughput << " probes/s\n";
  std::cout << std::setprecision(1);
  std::cout << "  Avg iterations/probe: " << stage2_avg_iter << "\n";
  std::cout << "  Avg deletions/probe: " << stage2_avg_del << "\n";

  std::cout << "------------------------------------------------\n";
  std::cout << std::setprecision(2);
  std::cout << "Stage 2 vs Stage 1: " << stage2_vs_stage1 << "x";
  if (stage2_vs_stage1 >= 1.0) {
    std::cout << " (Stage 2 FASTER)\n";
  } else {
    std::cout << " (Stage 1 faster)\n";
  }

  // 自动选择建议
  std::cout << "\n[Auto Selection Recommendation]\n";
  if (stage2_vs_stage1 >= 1.1) {
    std::cout << "  -> Use Stage 2 (Persistent Blocks): higher throughput\n";
  } else if (stage2_vs_stage1 <= 0.9) {
    std::cout << "  -> Use Stage 1 (Micro-Batch): lower overhead\n";
  } else {
    std::cout << "  -> Either stage is acceptable (within 10%)\n";
  }
  std::cout << "  Factors: num_tasks=" << total_probes
            << ", fail_rate=" << std::setprecision(1) << fail_rate << "%"
            << ", avg_iter=" << stage1_avg_iter << "\n";

  std::cout << "================================================\n";

  return 0;
}
