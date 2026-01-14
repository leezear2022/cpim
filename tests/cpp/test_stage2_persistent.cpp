// test_stage2_persistent.cpp
// 测试 Batch-2 Stage 2 (Persistent Blocks) 与 Stage 1 (Micro-Batch) 的正确性
//
// 验证：
// 1. Stage 1 和 Stage 2 产生相同的失败 probe 集合
// 2. Stage 2 的性能特性
//
// 用法：
//   ./test_stage2_persistent <instance.xml> [--num_blocks=N]

#include <chrono>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <vector>

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

// 将失败列表转换为集合
std::set<std::pair<int, int>> ToSet(const std::vector<int>& vars,
                                     const std::vector<int>& vals) {
  std::set<std::pair<int, int>> result;
  for (size_t i = 0; i < vars.size(); ++i) {
    result.insert({vars[i], vals[i]});
  }
  return result;
}

void PrintUsage(const char* prog) {
  std::cout << "Usage: " << prog << " <instance.xml> [--num_blocks=N] [--auto] [--chunk=C]\n";
  std::cout << "  --num_blocks=N : Number of persistent blocks (default: auto)\n";
  std::cout << "  --auto         : Enable auto-tuning (default: enabled)\n";
  std::cout << "  --no-auto      : Disable auto-tuning\n";
  std::cout << "  --chunk=C      : Chunk size for batch task pulling (default: 4)\n";
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    PrintUsage(argv[0]);
    return 1;
  }

  std::string input_path = argv[1];
  int num_blocks = -1;  // -1 表示自动
  bool auto_tune = true;
  int chunk_size = 1;   // 默认 chunk 大小（1 = 最佳性能）

  // 解析参数
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--num_blocks=", 0) == 0) {
      num_blocks = std::stoi(arg.substr(13));
      auto_tune = false;  // 指定了 num_blocks 则禁用 auto-tune
    } else if (arg == "--auto") {
      auto_tune = true;
      num_blocks = -1;
    } else if (arg == "--no-auto") {
      auto_tune = false;
      if (num_blocks == -1) num_blocks = 16;  // 默认值
    } else if (arg.rfind("--chunk=", 0) == 0) {
      chunk_size = std::stoi(arg.substr(8));
    }
  }

  std::cout << "========================================\n";
  std::cout << "Stage 1 vs Stage 2 Correctness Test\n";
  std::cout << "========================================\n";
  std::cout << "Instance: " << input_path << "\n";
  std::cout << "Persistent blocks: " << (num_blocks == -1 ? "auto" : std::to_string(num_blocks)) << "\n";
  std::cout << "Auto-tune: " << (auto_tune ? "enabled" : "disabled") << "\n";
  std::cout << "Chunk size: " << chunk_size << "\n";
  std::cout << "----------------------------------------\n";

  // 1. 加载并解析问题
  std::cout << "[1/5] Loading instance..." << std::flush;
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
  std::cout << "[2/5] Building GModel..." << std::flush;
  GModelOptions options;
  options.device_id = 0;
  GModel gmodel = GModelAdapter::Build(im_model, options);
  std::cout << " OK\n";

  // 3. 运行初始 GAC
  std::cout << "[3/5] Running initial GAC..." << std::flush;
  auto gac_stats = gmodel.EnforceGAC_Persistent(false, -1);
  std::cout << " OK (deletions=" << gac_stats.deletions << ")\n";

  // 4. 收集所有 probe 任务
  std::cout << "[4/5] Collecting probe tasks..." << std::flush;
  auto tasks = CollectAllProbeTasks(&gmodel);
  const int total_probes = static_cast<int>(tasks.size());
  std::cout << " OK (" << total_probes << " probes)\n";

  if (total_probes == 0) {
    std::cout << "\nNo probes to run (all variables assigned or empty domains)\n";
    return 0;
  }

  std::cout << "[5/5] Running Stage 1 and Stage 2...\n";
  std::cout << "----------------------------------------\n";

  // Stage 1: Micro-Batch
  std::cout << "\n=== Stage 1: Micro-Batch ===\n";
  Batch2ProbeManager stage1(&gmodel, 64);  // micro-batch size = 64
  stage1.SetActivationStrategy(1);  // NEIGHBOR_ACTIVATION

  for (const auto& t : tasks) {
    stage1.AddTask(t.var_id, t.value);
  }

  std::vector<int> stage1_failed_vars, stage1_failed_vals;
  auto start1 = std::chrono::high_resolution_clock::now();
  stage1.ExecuteMicroBatch(stage1_failed_vars, stage1_failed_vals);
  auto end1 = std::chrono::high_resolution_clock::now();

  double stage1_time =
      std::chrono::duration<double>(end1 - start1).count() * 1000;  // ms

  std::cout << "  Failed probes: " << stage1_failed_vars.size() << "\n";
  std::cout << "  Time: " << std::fixed << std::setprecision(3)
            << stage1_time << " ms\n";
  std::cout << "  Throughput: " << std::setprecision(0)
            << (total_probes / (stage1_time / 1000)) << " probes/s\n";

  // Stage 2: Persistent Blocks
  std::cout << "\n=== Stage 2: Persistent Blocks ===\n";
  Batch2PersistentManager stage2(&gmodel, num_blocks);
  if (!auto_tune && num_blocks != -1) {
    stage2.EnableAutoTune(false);
  }
  stage2.SetChunkSize(chunk_size);
  stage2.SetActivationStrategy(1);  // NEIGHBOR_ACTIVATION

  for (const auto& t : tasks) {
    stage2.AddTask(t.var_id, t.value);
  }

  std::vector<int> stage2_failed_vars, stage2_failed_vals;
  auto start2 = std::chrono::high_resolution_clock::now();
  stage2.ExecutePersistentBlocks(stage2_failed_vars, stage2_failed_vals);
  auto end2 = std::chrono::high_resolution_clock::now();

  double stage2_time =
      std::chrono::duration<double>(end2 - start2).count() * 1000;  // ms

  std::cout << "  Effective blocks: " << stage2.GetEffectiveNumBlocks() << "\n";
  std::cout << "  Failed probes: " << stage2_failed_vars.size() << "\n";
  std::cout << "  Time: " << std::fixed << std::setprecision(3)
            << stage2_time << " ms\n";
  std::cout << "  Throughput: " << std::setprecision(0)
            << (total_probes / (stage2_time / 1000)) << " probes/s\n";

  // 验证结果一致性
  std::cout << "\n=== Correctness Verification ===\n";

  auto stage1_set = ToSet(stage1_failed_vars, stage1_failed_vals);
  auto stage2_set = ToSet(stage2_failed_vars, stage2_failed_vals);

  bool correct = (stage1_set == stage2_set);

  if (correct) {
    std::cout << "✅ PASS: Stage 1 and Stage 2 produce identical results\n";
  } else {
    std::cout << "❌ FAIL: Results differ!\n";

    // 找出差异
    std::set<std::pair<int, int>> only_in_stage1, only_in_stage2;

    for (const auto& p : stage1_set) {
      if (stage2_set.find(p) == stage2_set.end()) {
        only_in_stage1.insert(p);
      }
    }
    for (const auto& p : stage2_set) {
      if (stage1_set.find(p) == stage1_set.end()) {
        only_in_stage2.insert(p);
      }
    }

    if (!only_in_stage1.empty()) {
      std::cout << "  Only in Stage 1 (" << only_in_stage1.size() << "):\n";
      int count = 0;
      for (const auto& p : only_in_stage1) {
        if (count++ < 5) {
          std::cout << "    var=" << p.first << ", val=" << p.second << "\n";
        }
      }
      if (only_in_stage1.size() > 5) {
        std::cout << "    ... and " << (only_in_stage1.size() - 5) << " more\n";
      }
    }

    if (!only_in_stage2.empty()) {
      std::cout << "  Only in Stage 2 (" << only_in_stage2.size() << "):\n";
      int count = 0;
      for (const auto& p : only_in_stage2) {
        if (count++ < 5) {
          std::cout << "    var=" << p.first << ", val=" << p.second << "\n";
        }
      }
      if (only_in_stage2.size() > 5) {
        std::cout << "    ... and " << (only_in_stage2.size() - 5) << " more\n";
      }
    }
  }

  // 性能对比
  std::cout << "\n=== Performance Summary ===\n";
  std::cout << "Total probes: " << total_probes << "\n";
  std::cout << std::setprecision(3);
  std::cout << "Stage 1 (Micro-Batch): " << stage1_time << " ms\n";
  std::cout << "Stage 2 (Persistent):  " << stage2_time << " ms\n";
  std::cout << std::setprecision(2);
  std::cout << "Speedup (Stage 2 / Stage 1): "
            << (stage1_time / stage2_time) << "x\n";
  std::cout << "========================================\n";

  return correct ? 0 : 1;
}
