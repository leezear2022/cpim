// Copyright 2025 CPIM Project
// Unit Test: BatchProbeManager State Consistency
//
// 验证 ExecuteBatch 前后 GModel 状态完全一致
// - bitDom 不变
// - d_cur_dom_size 不变
//
// 测试策略：
// 1. 加载测试实例（queens-4）
// 2. 保存初始域状态
// 3. 执行 BatchProbeManager::ExecuteBatch
// 4. 验证域状态与初始状态完全一致

#include <glog/logging.h>
#include <gflags/gflags.h>
#include <vector>
#include <cstring>

#include "GModel.cuh"
#include "model/gmodel_adapter.h"
#include "model/intermediate_model.h"
#include "model/model_normalizer.h"
#include "model/xcsp_parser.h"
#include "solver/gpu/batch_probe_manager.h"

// 项目根路径宏（由 CMakeLists.txt 定义）
#ifndef CPIM_PROJECT_ROOT
#define CPIM_PROJECT_ROOT "."
#endif

DEFINE_string(input, CPIM_PROJECT_ROOT "/tests/data/bench/queens-4_ext.xml",
              "Input XCSP3 file path");

namespace cpim {

using namespace cpim::model;

// 辅助函数：比较两个数组是否相等
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

// 辅助函数：深拷贝 GPU 数组到 Host
template <typename T>
std::vector<T> CopyFromDevice(const T* d_ptr, int count) {
  std::vector<T> host_copy(count);
  cudaError_t err = cudaMemcpy(host_copy.data(), d_ptr,
                                count * sizeof(T),
                                cudaMemcpyDeviceToHost);
  CHECK(err == cudaSuccess) << "cudaMemcpy failed: "
                            << cudaGetErrorString(err);
  return host_copy;
}

// 辅助函数：测试单个策略
bool TestSingleStrategy(GModel* gmodel, int strategy,
                        std::vector<int>& failed_vars_out,
                        std::vector<int>& failed_values_out) {
  const int num_vars = gmodel->GetNumVars();
  const int bit_dom_int_size = gmodel->GetBitDomIntSize();
  const int snapshot_size_words = num_vars * bit_dom_int_size;

  const char* strategy_name = (strategy == 0) ? "FULL_ACTIVATION" : "NEIGHBOR_ACTIVATION";

  LOG(INFO) << "Testing strategy: " << strategy_name;

  // 保存初始域状态
  auto initial_bitDom = CopyFromDevice(gmodel->GetBitDom(), snapshot_size_words);
  auto initial_dom_sizes = CopyFromDevice(gmodel->GetDomainSizesPtr(), num_vars);

  // 创建 BatchProbeManager
  BatchProbeManager batch_mgr(gmodel, 16);
  batch_mgr.SetActivationStrategy(strategy);

  // 添加探测任务
  int num_tasks = 0;
  for (int var = 0; var < num_vars; ++var) {
    if (gmodel->IsAssigned(var)) continue;

    for (int val = gmodel->GetFirstValue(var);
         val != -1;
         val = gmodel->GetNextValue(var, val)) {
      batch_mgr.AddTask(var, val);
      num_tasks++;
    }
  }

  LOG(INFO) << "Added " << num_tasks << " probe tasks";

  // 执行批量探测
  std::vector<int> failed_vars, failed_values;
  int num_failed = batch_mgr.ExecuteBatch(failed_vars, failed_values);

  LOG(INFO) << strategy_name << " result: " << num_failed << " / " << num_tasks
            << " probes failed";

  // 验证状态一致性
  auto final_bitDom = CopyFromDevice(gmodel->GetBitDom(), snapshot_size_words);
  bool bitDom_ok = ArraysEqual(final_bitDom.data(),
                                initial_bitDom.data(),
                                snapshot_size_words,
                                "bitDom");

  auto final_dom_sizes = CopyFromDevice(gmodel->GetDomainSizesPtr(), num_vars);
  bool dom_sizes_ok = ArraysEqual(final_dom_sizes.data(),
                                   initial_dom_sizes.data(),
                                   num_vars,
                                   "d_cur_dom_size");

  bool success = bitDom_ok && dom_sizes_ok;

  if (success) {
    LOG(INFO) << "✓ " << strategy_name << " state consistency: PASS";
  } else {
    LOG(ERROR) << "✗ " << strategy_name << " state consistency: FAIL";
  }

  // 输出失败的探测
  failed_vars_out = failed_vars;
  failed_values_out = failed_values;

  return success;
}

// 主测试函数
bool TestBatchProbeStateConsistency() {
  LOG(INFO) << "========================================";
  LOG(INFO) << "Test: BatchProbeManager State Consistency";
  LOG(INFO) << "========================================";

  // 1. 加载测试实例
  LOG(INFO) << "Loading instance: " << FLAGS_input;
  auto parser = XcspParser::Create(ParserType::kLibXml2);
  auto model_or = parser->Parse(FLAGS_input);

  if (!model_or.ok()) {
    LOG(ERROR) << "Failed to parse model: " << model_or.status();
    return false;
  }

  // 2. 归一化模型
  ModelNormalizer normalizer;
  auto normalized_or = normalizer.Normalize(*model_or);
  if (!normalized_or.ok()) {
    LOG(ERROR) << "Normalization failed: " << normalized_or.status();
    return false;
  }
  const auto& im_model = *normalized_or;

  // 3. 构建 GModel
  LOG(INFO) << "Building GModel...";
  auto gmodel_obj = model::GModelAdapter::Build(im_model);
  GModel* gmodel = &gmodel_obj;

  const int num_vars = gmodel->GetNumVars();
  const int bit_dom_int_size = gmodel->GetBitDomIntSize();
  const int snapshot_size_words = num_vars * bit_dom_int_size;

  LOG(INFO) << "GModel: " << num_vars << " vars, "
            << snapshot_size_words << " bitDom words";

  // 4. 初始 GAC 传播（确保模型处于一致状态）
  LOG(INFO) << "Running initial GAC propagation...";
  auto gac_stats = gmodel->EnforceGAC_Persistent(false, -1);

  if (gac_stats.inconsistent) {
    LOG(INFO) << "Initial GAC detected inconsistency (UNSAT instance)";
    LOG(INFO) << "Skipping probe tests for UNSAT instance";
    LOG(INFO) << "========================================";
    LOG(INFO) << "✓ Test skipped (UNSAT instance)";
    return true;  // UNSAT 是预期行为，返回 true
  }

  LOG(INFO) << "Initial GAC: " << gac_stats.deletions << " deletions, "
            << gac_stats.iterations << " iterations";

  // 5. 测试两种策略并对比结果
  LOG(INFO) << "";
  LOG(INFO) << "========================================";
  LOG(INFO) << "Phase 1: Testing FULL_ACTIVATION";
  LOG(INFO) << "========================================";

  std::vector<int> full_failed_vars, full_failed_values;
  bool full_ok = TestSingleStrategy(gmodel, 0, full_failed_vars, full_failed_values);

  LOG(INFO) << "";
  LOG(INFO) << "========================================";
  LOG(INFO) << "Phase 2: Testing NEIGHBOR_ACTIVATION";
  LOG(INFO) << "========================================";

  std::vector<int> neighbor_failed_vars, neighbor_failed_values;
  bool neighbor_ok = TestSingleStrategy(gmodel, 1, neighbor_failed_vars, neighbor_failed_values);

  // 6. 对比两种策略的结果
  LOG(INFO) << "";
  LOG(INFO) << "========================================";
  LOG(INFO) << "Phase 3: Comparing Results";
  LOG(INFO) << "========================================";

  bool results_match = (full_failed_vars.size() == neighbor_failed_vars.size());

  if (results_match) {
    // 对比每个失败的探测
    std::sort(full_failed_vars.begin(), full_failed_vars.end());
    std::sort(neighbor_failed_vars.begin(), neighbor_failed_vars.end());

    for (size_t i = 0; i < full_failed_vars.size(); ++i) {
      if (full_failed_vars[i] != neighbor_failed_vars[i] ||
          full_failed_values[i] != neighbor_failed_values[i]) {
        results_match = false;
        LOG(ERROR) << "Mismatch at index " << i
                   << ": FULL=(" << full_failed_vars[i] << "," << full_failed_values[i] << "), "
                   << "NEIGHBOR=(" << neighbor_failed_vars[i] << "," << neighbor_failed_values[i] << ")";
        break;
      }
    }
  } else {
    LOG(ERROR) << "Different number of failures: "
               << "FULL=" << full_failed_vars.size()
               << ", NEIGHBOR=" << neighbor_failed_vars.size();
  }

  // 7. 总结
  LOG(INFO) << "========================================";
  LOG(INFO) << "Final Results:";
  LOG(INFO) << "  FULL_ACTIVATION state:      " << (full_ok ? "✓ PASS" : "✗ FAIL");
  LOG(INFO) << "  NEIGHBOR_ACTIVATION state:  " << (neighbor_ok ? "✓ PASS" : "✗ FAIL");
  LOG(INFO) << "  Results consistency:        " << (results_match ? "✓ PASS" : "✗ FAIL");
  LOG(INFO) << "  Failed probes: " << full_failed_vars.size()
            << " (FULL) vs " << neighbor_failed_vars.size() << " (NEIGHBOR)";
  LOG(INFO) << "========================================";

  bool success = full_ok && neighbor_ok && results_match;

  if (success) {
    LOG(INFO) << "✓ ALL TESTS PASSED";
  } else {
    LOG(ERROR) << "✗ SOME TESTS FAILED";
  }

  return success;
}

}  // namespace cpim

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  FLAGS_logtostderr = 1;  // 输出到控制台
  FLAGS_v = 1;            // VLOG level 1

  bool success = cpim::TestBatchProbeStateConsistency();

  return success ? 0 : 1;
}
