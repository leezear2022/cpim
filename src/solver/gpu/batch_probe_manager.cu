// Copyright 2025 CPIM Project
// Batch Probe Manager Implementation
// GPU-accelerated SAC using Batch-1 approach

#include "solver/gpu/batch_probe_manager.h"
#include <glog/logging.h>
#include <cuda_runtime.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace cpim {

namespace {

int SanitizeSubwarpSize(int subwarp_size) {
  switch (subwarp_size) {
    case 4:
    case 8:
    case 16:
      return subwarp_size;
    default:
      return 8;
  }
}

}  // namespace

// ============================================================================
// 构造函数
// ============================================================================
BatchProbeManager::BatchProbeManager(GModel* model, int max_batch_size)
    : model_(model),
      max_batch_size_(max_batch_size),
      d_tasks_(nullptr),
      d_results_(nullptr),
      d_snapshot_(nullptr),
      d_dom_size_snapshot_(nullptr),
      d_control_(nullptr),
      d_frontier_A_(nullptr),
      d_frontier_B_(nullptr),
      memory_allocated_(false) {
  CHECK(model != nullptr) << "GModel pointer cannot be null";
  CHECK(max_batch_size > 0) << "max_batch_size must be positive";

  AllocateMemory();
}

// ============================================================================
// 析构函数
// ============================================================================
BatchProbeManager::~BatchProbeManager() {
  FreeMemory();
}

// ============================================================================
// 分配 GPU 内存
// ============================================================================
void BatchProbeManager::AllocateMemory() {
  if (memory_allocated_) {
    LOG(WARNING) << "Memory already allocated, skipping...";
    return;
  }

  const int num_vars = model_->GetNumVars();
  const int bit_dom_int_size = model_->GetBitDomIntSize();
  const int num_cons = model_->GetNumCons();

  // 计算域快照大小（words）
  const int snapshot_size_words = num_vars * bit_dom_int_size;

  // 计算前沿位图大小（words）
  const int bitmap_size_words = (num_cons + 31) / 32;

  LOG(INFO) << "Allocating BatchProbeManager memory:";
  LOG(INFO) << "  max_batch_size: " << max_batch_size_;
  LOG(INFO) << "  num_vars: " << num_vars;
  LOG(INFO) << "  snapshot_size_words: " << snapshot_size_words;
  LOG(INFO) << "  bitmap_size_words: " << bitmap_size_words;

  // 分配统一内存（Jetson 友好）
  cudaError_t err;

  // 任务数组 [max_batch_size]
  err = cudaMallocManaged(&d_tasks_, max_batch_size_ * sizeof(ProbeTask));
  CHECK(err == cudaSuccess) << "Failed to allocate d_tasks_: "
                            << cudaGetErrorString(err);

  // 结果数组 [max_batch_size]
  err = cudaMallocManaged(&d_results_, max_batch_size_ * sizeof(bool));
  CHECK(err == cudaSuccess) << "Failed to allocate d_results_: "
                            << cudaGetErrorString(err);

  // 域快照 [num_vars * bit_dom_int_size]
  err = cudaMallocManaged(&d_snapshot_, snapshot_size_words * sizeof(u32));
  CHECK(err == cudaSuccess) << "Failed to allocate d_snapshot_: "
                            << cudaGetErrorString(err);

  // 域大小快照 [num_vars]
  err = cudaMallocManaged(&d_dom_size_snapshot_, num_vars * sizeof(int));
  CHECK(err == cudaSuccess) << "Failed to allocate d_dom_size_snapshot_: "
                            << cudaGetErrorString(err);

  // 控制块
  err = cudaMallocManaged(&d_control_, sizeof(BatchProbeControl));
  CHECK(err == cudaSuccess) << "Failed to allocate d_control_: "
                            << cudaGetErrorString(err);

  // 前沿位图 A
  err = cudaMallocManaged(&d_frontier_A_, bitmap_size_words * sizeof(u32));
  CHECK(err == cudaSuccess) << "Failed to allocate d_frontier_A_: "
                            << cudaGetErrorString(err);

  // 前沿位图 B
  err = cudaMallocManaged(&d_frontier_B_, bitmap_size_words * sizeof(u32));
  CHECK(err == cudaSuccess) << "Failed to allocate d_frontier_B_: "
                            << cudaGetErrorString(err);

  // 初始化控制块
  d_control_->bitmap_size_words = bitmap_size_words;
  d_control_->frontier_A = d_frontier_A_;
  d_control_->frontier_B = d_frontier_B_;
  d_control_->domain_snapshot = d_snapshot_;

  memory_allocated_ = true;

  LOG(INFO) << "Memory allocated successfully";
}

// ============================================================================
// 释放 GPU 内存
// ============================================================================
void BatchProbeManager::FreeMemory() {
  if (!memory_allocated_) {
    return;
  }

  if (d_tasks_) cudaFree(d_tasks_);
  if (d_results_) cudaFree(d_results_);
  if (d_snapshot_) cudaFree(d_snapshot_);
  if (d_dom_size_snapshot_) cudaFree(d_dom_size_snapshot_);
  if (d_control_) cudaFree(d_control_);
  if (d_frontier_A_) cudaFree(d_frontier_A_);
  if (d_frontier_B_) cudaFree(d_frontier_B_);

  d_tasks_ = nullptr;
  d_results_ = nullptr;
  d_snapshot_ = nullptr;
  d_dom_size_snapshot_ = nullptr;
  d_control_ = nullptr;
  d_frontier_A_ = nullptr;
  d_frontier_B_ = nullptr;

  memory_allocated_ = false;

  LOG(INFO) << "Memory freed successfully";
}

// ============================================================================
// 添加探测任务
// ============================================================================
void BatchProbeManager::AddTask(int var_id, int value) {
  // 队列满检查
  if (task_queue_.size() >= static_cast<size_t>(max_batch_size_)) {
    LOG(WARNING) << "Task queue full (" << max_batch_size_
                 << "), consider calling ExecuteBatch() first";
    return;
  }

  // 变量 ID 边界检查
  const int num_vars = model_->GetNumVars();
  if (var_id < 0 || var_id >= num_vars) {
    LOG(ERROR) << "Invalid var_id=" << var_id
               << " (valid range: 0-" << (num_vars - 1) << ")";
    return;
  }

  // 值范围校验（DEBUG 模式，避免性能影响）
  #ifdef CPIM_DEBUG_MODE
  bool value_valid = false;
  for (int val = model_->GetFirstValue(var_id); val != -1;
       val = model_->GetNextValue(var_id, val)) {
    if (val == value) {
      value_valid = true;
      break;
    }
  }

  if (!value_valid) {
    LOG(ERROR) << "Invalid value=" << value << " for var_id=" << var_id
               << " (not in domain, size=" << model_->GetDomainSize(var_id) << ")";
    return;
  }
  #endif

  task_queue_.emplace_back(var_id, value, task_queue_.size());
}

// ============================================================================
// 清空任务队列
// ============================================================================
void BatchProbeManager::Clear() {
  task_queue_.clear();
}

// ============================================================================
// 设置 Frontier 初始化策略
// ============================================================================
void BatchProbeManager::SetActivationStrategy(int strategy) {
  CHECK(strategy == 0 || strategy == 1)
      << "Invalid activation strategy: " << strategy
      << " (must be 0=FULL or 1=NEIGHBOR)";

  if (memory_allocated_) {
    d_control_->activation_strategy = strategy;
    cudaDeviceSynchronize();
    VLOG(1) << "Activation strategy set to "
            << (strategy == 0 ? "FULL_ACTIVATION" : "NEIGHBOR_ACTIVATION");
  } else {
    LOG(WARNING) << "Memory not allocated yet, strategy will be set on first ExecuteBatch";
  }
}

// ============================================================================
// 保存当前域快照
// ============================================================================
void BatchProbeManager::SaveSnapshot() {
  const int num_vars = model_->GetNumVars();
  const int bit_dom_int_size = model_->GetBitDomIntSize();
  const int snapshot_size_words = num_vars * bit_dom_int_size;

  // 从 GModel 获取当前域指针
  const u32* current_domain = model_->GetBitDom();

  // 拷贝 bitDom 到快照缓冲区
  cudaError_t err = cudaMemcpy(d_snapshot_, current_domain,
                                snapshot_size_words * sizeof(u32),
                                cudaMemcpyDeviceToDevice);
  CHECK(err == cudaSuccess) << "Failed to save domain snapshot: "
                            << cudaGetErrorString(err);

  // 拷贝 d_cur_dom_size 到快照缓冲区（修复高优先级bug）
  const int* current_dom_sizes = model_->GetDomainSizesPtr();
  err = cudaMemcpy(d_dom_size_snapshot_, current_dom_sizes,
                    num_vars * sizeof(int),
                    cudaMemcpyDeviceToDevice);
  CHECK(err == cudaSuccess) << "Failed to save domain sizes snapshot: "
                            << cudaGetErrorString(err);

  VLOG(2) << "Domain snapshot saved (bitDom: " << snapshot_size_words
          << " words, dom_sizes: " << num_vars << " ints)";

  // P0-1: 设置 frontier 初始化策略
  // 假设快照总是 AC（调用者保证）
  d_control_->snapshot_is_ac = true;
  // 默认使用 NEIGHBOR_ACTIVATION（可通过 SetActivationStrategy 修改）
  if (d_control_->activation_strategy == -1) {
    // 如果还没设置过策略，使用默认值
    d_control_->activation_strategy = 1;  // NEIGHBOR_ACTIVATION
    VLOG(2) << "Using default activation strategy: NEIGHBOR_ACTIVATION";
  } else {
    VLOG(2) << "Using user-specified activation strategy: "
            << (d_control_->activation_strategy == 0 ? "FULL_ACTIVATION" : "NEIGHBOR_ACTIVATION");
  }

  cudaDeviceSynchronize();
}

// ============================================================================
// 恢复域快照（修复高优先级bug）
// ============================================================================
void BatchProbeManager::RestoreSnapshot() {
  const int num_vars = model_->GetNumVars();
  const int bit_dom_int_size = model_->GetBitDomIntSize();
  const int snapshot_size_words = num_vars * bit_dom_int_size;

  // 恢复 bitDom（使用 GetBitDomMutable 避免 const_cast）
  u32* current_domain = model_->GetBitDomMutable();
  cudaError_t err = cudaMemcpy(current_domain, d_snapshot_,
                                snapshot_size_words * sizeof(u32),
                                cudaMemcpyDeviceToDevice);
  CHECK(err == cudaSuccess) << "Failed to restore domain snapshot: "
                            << cudaGetErrorString(err);

  // 恢复 d_cur_dom_size（使用 GModel 的公开接口）
  model_->RestoreDomainSizes(d_dom_size_snapshot_, num_vars);

  VLOG(2) << "Domain snapshot restored (bitDom: " << snapshot_size_words
          << " words, dom_sizes: " << num_vars << " ints)";
}

// ============================================================================
// 启动批量探测 kernel（前向声明，实际实现在 GModel.cu）
// ============================================================================
// 这个函数会在 GModel.cu 中实现，这里只做前向声明
// grid_size 和 block_size 会在 wrapper 内部根据设备能力自动计算
extern void LaunchPersistentBatchProbeKernelWrapper(
    GModelData model_data,
    BatchProbeControl* control,
    int max_iterations_per_probe);

void BatchProbeManager::LaunchBatchProbeKernel(int num_tasks) {
  // 初始化控制块
  d_control_->num_tasks = num_tasks;
  d_control_->current_task_index = 0;
  d_control_->tasks = d_tasks_;
  d_control_->results = d_results_;
  d_control_->dom_size_snapshot = d_dom_size_snapshot_;  // 域大小快照指针

  // 清空结果数组
  cudaMemset(d_results_, 0, num_tasks * sizeof(bool));

  // 清空前沿位图
  const int bitmap_size_bytes = d_control_->bitmap_size_words * sizeof(u32);
  cudaMemset(d_frontier_A_, 0, bitmap_size_bytes);
  cudaMemset(d_frontier_B_, 0, bitmap_size_bytes);

  // 获取 GModel 数据
  GModelData model_data = model_->GetModelData();

  // Kernel 参数（参考 PersistentGACKernel 的配置）
  const int max_iterations_per_probe = 1000;  // 每个 probe 的最大迭代次数

  LOG(INFO) << "Launching BatchProbeKernel:";
  LOG(INFO) << "  num_tasks: " << num_tasks;
  LOG(INFO) << "  max_iterations: " << max_iterations_per_probe;

  // 调用 wrapper 函数（实际 kernel 在 GModel.cu 中，会自动计算 grid/block）
  LaunchPersistentBatchProbeKernelWrapper(
      model_data, d_control_, max_iterations_per_probe);

  // 同步等待 kernel 完成
  cudaError_t err = cudaDeviceSynchronize();
  CHECK(err == cudaSuccess) << "Kernel execution failed: "
                            << cudaGetErrorString(err);

  VLOG(1) << "BatchProbeKernel completed successfully";
}

// ============================================================================
// 收集结果
// ============================================================================
int BatchProbeManager::CollectResults(int num_tasks,
                                       std::vector<int>& failed_vars,
                                       std::vector<int>& failed_values) {
  failed_vars.clear();
  failed_values.clear();

  int num_failed = 0;

  for (int i = 0; i < num_tasks; ++i) {
    // results[i] = true 表示一致（该值有效）
    // results[i] = false 表示 DWO（该值应删除）
    if (!d_results_[i]) {
      failed_vars.push_back(d_tasks_[i].var_id);
      failed_values.push_back(d_tasks_[i].value);
      num_failed++;
    }
  }

  LOG(INFO) << "Collected results: " << num_failed << " / " << num_tasks
            << " tasks failed";

  return num_failed;
}

// ============================================================================
// 执行批量探测
// ============================================================================
int BatchProbeManager::ExecuteBatch(std::vector<int>& failed_vars,
                                     std::vector<int>& failed_values) {
  const int num_tasks = task_queue_.size();

  if (num_tasks == 0) {
    LOG(WARNING) << "No tasks to execute";
    failed_vars.clear();
    failed_values.clear();
    return 0;
  }

  LOG(INFO) << "ExecuteBatch: processing " << num_tasks << " tasks";

  // 步骤 1: 保存域快照
  SaveSnapshot();

  // 步骤 2: 拷贝任务到 GPU
  cudaError_t err = cudaMemcpy(d_tasks_, task_queue_.data(),
                                num_tasks * sizeof(ProbeTask),
                                cudaMemcpyHostToDevice);
  CHECK(err == cudaSuccess) << "Failed to copy tasks to device: "
                            << cudaGetErrorString(err);

  // 步骤 3: 启动批量探测 kernel
  LaunchBatchProbeKernel(num_tasks);

  // 步骤 4: 收集结果
  int num_failed = CollectResults(num_tasks, failed_vars, failed_values);

  // 步骤 5: 恢复原始域状态（修复高优先级bug）
  RestoreSnapshot();

  // 步骤 6: 清空任务队列
  Clear();

  // P0-2: 输出 Precheck 统计
  if (d_control_->precheck_count > 0) {
    double short_circuit_rate = 100.0 * d_control_->short_circuit_count / d_control_->precheck_count;
    std::cout << "[BatchProbe] Precheck 统计: "
              << "总次数=" << d_control_->precheck_count
              << ", 短路=" << d_control_->short_circuit_count
              << " (" << std::fixed << std::setprecision(1) << short_circuit_rate << "%)"
              << std::endl;
  }

  // 重置统计（为下次调用做准备）
  d_control_->precheck_count = 0;
  d_control_->short_circuit_count = 0;

  return num_failed;
}

// ============================================================================
// Batch-2: Host 端管理器（Stage 1: Micro-Batch）
// ============================================================================

Batch2ProbeManager::Batch2ProbeManager(GModel* model, int max_batch_size)
    : model_(model),
      max_batch_size_(max_batch_size) {
  CHECK(model_ != nullptr) << "GModel pointer cannot be null";
  CHECK(max_batch_size_ > 0) << "max_batch_size must be positive";

  AllocateMemory();
}

Batch2ProbeManager::~Batch2ProbeManager() {
  FreeMemory();
}

void Batch2ProbeManager::AllocateMemory() {
  if (memory_allocated_) {
    return;
  }

  const int num_vars = model_->GetNumVars();
  const int bit_dom_int_size = model_->GetBitDomIntSize();
  const int num_cons = model_->GetNumCons();

  const int dom_words_per_world = num_vars * bit_dom_int_size;
  const int bitmap_size_words = (num_cons + 31) / 32;

  const size_t snapshot_bytes =
      static_cast<size_t>(dom_words_per_world) * sizeof(u32);
  const size_t dom_size_bytes = static_cast<size_t>(num_vars) * sizeof(int);
  const size_t tasks_bytes =
      static_cast<size_t>(max_batch_size_) * sizeof(ProbeTask);
  const size_t results_bytes =
      static_cast<size_t>(max_batch_size_) * sizeof(bool);

  const size_t ws_struct_bytes =
      static_cast<size_t>(max_batch_size_) * sizeof(WorldWorkspace);
  const size_t ws_bitdom_bytes =
      static_cast<size_t>(max_batch_size_) *
      static_cast<size_t>(dom_words_per_world) * sizeof(u32);
  const size_t ws_dom_size_bytes =
      static_cast<size_t>(max_batch_size_) * static_cast<size_t>(num_vars) *
      sizeof(int);
  const size_t ws_frontier_bytes =
      static_cast<size_t>(max_batch_size_) *
      static_cast<size_t>(bitmap_size_words) * sizeof(u32);

  LOG(INFO) << "Allocating Batch2ProbeManager memory (Micro-Batch):";
  LOG(INFO) << "  max_batch_size: " << max_batch_size_;
  LOG(INFO) << "  num_vars: " << num_vars;
  LOG(INFO) << "  num_cons: " << num_cons;
  LOG(INFO) << "  bit_dom_int_size: " << bit_dom_int_size;
  LOG(INFO) << "  bitmap_size_words: " << bitmap_size_words;
  LOG(INFO) << "  snapshot_bytes: " << snapshot_bytes;
  LOG(INFO) << "  ws_bitdom_bytes: " << ws_bitdom_bytes;
  LOG(INFO) << "  ws_dom_size_bytes: " << ws_dom_size_bytes;
  LOG(INFO) << "  ws_frontier_bytes(A): " << ws_frontier_bytes;
  LOG(INFO) << "  ws_frontier_bytes(B): " << ws_frontier_bytes;

  cudaError_t err;

  err = cudaMallocManaged(&d_tasks_, tasks_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_tasks_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_results_, results_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_results_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_snapshot_, snapshot_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_snapshot_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_dom_size_snapshot_, dom_size_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_dom_size_snapshot_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_workspaces_, ws_struct_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_workspaces_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_ws_bitdom_, ws_bitdom_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_ws_bitdom_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_ws_dom_size_, ws_dom_size_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_ws_dom_size_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_ws_frontier_A_, ws_frontier_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_ws_frontier_A_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_ws_frontier_B_, ws_frontier_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_ws_frontier_B_: "
                            << cudaGetErrorString(err);

  // 初始化每个 workspace 的切片指针
  for (int i = 0; i < max_batch_size_; ++i) {
    WorldWorkspace& ws = d_workspaces_[i];
    ws.bitDom = d_ws_bitdom_ + static_cast<size_t>(i) * dom_words_per_world;
    ws.d_cur_dom_size = d_ws_dom_size_ + static_cast<size_t>(i) * num_vars;
    ws.frontier_A =
        d_ws_frontier_A_ + static_cast<size_t>(i) * bitmap_size_words;
    ws.frontier_B =
        d_ws_frontier_B_ + static_cast<size_t>(i) * bitmap_size_words;
    ws.inconsistent_flag = 0;
    ws.scanner_index = 0;
    ws.deletions = 0;
    ws.iterations = 0;
    ws.frontier_nonempty = 0;
  }

  memory_allocated_ = true;
}

void Batch2ProbeManager::FreeMemory() {
  if (!memory_allocated_) {
    return;
  }

  if (d_tasks_) cudaFree(d_tasks_);
  if (d_results_) cudaFree(d_results_);
  if (d_snapshot_) cudaFree(d_snapshot_);
  if (d_dom_size_snapshot_) cudaFree(d_dom_size_snapshot_);
  if (d_workspaces_) cudaFree(d_workspaces_);
  if (d_ws_bitdom_) cudaFree(d_ws_bitdom_);
  if (d_ws_dom_size_) cudaFree(d_ws_dom_size_);
  if (d_ws_frontier_A_) cudaFree(d_ws_frontier_A_);
  if (d_ws_frontier_B_) cudaFree(d_ws_frontier_B_);

  d_tasks_ = nullptr;
  d_results_ = nullptr;
  d_snapshot_ = nullptr;
  d_dom_size_snapshot_ = nullptr;
  d_workspaces_ = nullptr;
  d_ws_bitdom_ = nullptr;
  d_ws_dom_size_ = nullptr;
  d_ws_frontier_A_ = nullptr;
  d_ws_frontier_B_ = nullptr;

  memory_allocated_ = false;
}

void Batch2ProbeManager::AddTask(int var_id, int value) {
  const int num_vars = model_->GetNumVars();
  if (var_id < 0 || var_id >= num_vars) {
    LOG(ERROR) << "Invalid var_id=" << var_id
               << " (valid range: 0-" << (num_vars - 1) << ")";
    return;
  }

  #ifdef CPIM_DEBUG_MODE
  bool value_valid = false;
  for (int val = model_->GetFirstValue(var_id); val != -1;
       val = model_->GetNextValue(var_id, val)) {
    if (val == value) {
      value_valid = true;
      break;
    }
  }
  if (!value_valid) {
    LOG(ERROR) << "Invalid value=" << value << " for var_id=" << var_id
               << " (not in domain, size=" << model_->GetDomainSize(var_id)
               << ")";
    return;
  }
  #endif

  task_queue_.emplace_back(var_id, value, task_queue_.size());
}

void Batch2ProbeManager::Clear() {
  task_queue_.clear();
}

void Batch2ProbeManager::SetActivationStrategy(int strategy) {
  CHECK(strategy == 0 || strategy == 1)
      << "Invalid activation strategy: " << strategy
      << " (must be 0=FULL or 1=NEIGHBOR)";
  activation_strategy_ = strategy;
}

void Batch2ProbeManager::EnablePrecheck(bool enabled) {
  precheck_enabled_ = enabled;
}

void Batch2ProbeManager::EnableStats(bool enabled) {
  stats_enabled_ = enabled;
}

void Batch2ProbeManager::SaveSnapshot() {
  const int num_vars = model_->GetNumVars();
  const int bit_dom_int_size = model_->GetBitDomIntSize();
  const int snapshot_size_words = num_vars * bit_dom_int_size;

  const u32* current_domain = model_->GetBitDom();
  cudaError_t err = cudaMemcpy(d_snapshot_, current_domain,
                                snapshot_size_words * sizeof(u32),
                                cudaMemcpyDeviceToDevice);
  CHECK(err == cudaSuccess) << "Failed to save domain snapshot (bitDom): "
                            << cudaGetErrorString(err);

  const int* current_dom_sizes = model_->GetDomainSizesPtr();
  err = cudaMemcpy(d_dom_size_snapshot_, current_dom_sizes,
                    num_vars * sizeof(int),
                    cudaMemcpyDeviceToDevice);
  CHECK(err == cudaSuccess) << "Failed to save domain sizes snapshot: "
                            << cudaGetErrorString(err);

  err = cudaDeviceSynchronize();
  CHECK(err == cudaSuccess) << "cudaDeviceSynchronize failed after SaveSnapshot: "
                            << cudaGetErrorString(err);
}

// 前向声明：Batch-2 micro-batch wrapper（实现在 GModel.cu）
extern void LaunchBatch2MicroBatchKernelWrapper(
    GModelData model_data,
    const u32* domain_snapshot,
    const int* dom_size_snapshot,
    const ProbeTask* tasks,
    bool* results,
    WorldWorkspace* workspaces,
    int batch_size,
    int max_iterations_per_probe,
    int activation_strategy,
    int enable_precheck);

void Batch2ProbeManager::LaunchMicroBatchKernel(int batch_size) {
  CHECK(batch_size > 0) << "batch_size must be positive";
  CHECK(batch_size <= max_batch_size_)
      << "batch_size " << batch_size << " exceeds max_batch_size "
      << max_batch_size_;

  // 获取 GModel 数据
  const GModelData model_data = model_->GetModelData();
  const int max_iterations_per_probe = 1000;

  LaunchBatch2MicroBatchKernelWrapper(
      model_data,
      d_snapshot_,
      d_dom_size_snapshot_,
      d_tasks_,
      d_results_,
      d_workspaces_,
      batch_size,
      max_iterations_per_probe,
      activation_strategy_,
      precheck_enabled_ ? 1 : 0);

  cudaError_t err = cudaDeviceSynchronize();
  CHECK(err == cudaSuccess) << "Batch2 micro-batch kernel failed: "
                            << cudaGetErrorString(err);
}

int Batch2ProbeManager::CollectBatchResults(
    int batch_size,
    int task_offset,
    std::vector<int>& failed_vars,
    std::vector<int>& failed_values) {
  int num_failed = 0;
  for (int i = 0; i < batch_size; ++i) {
    if (!d_results_[i]) {
      const ProbeTask& t = task_queue_[task_offset + i];
      failed_vars.push_back(t.var_id);
      failed_values.push_back(t.value);
      num_failed++;
    }
  }
  return num_failed;
}

int Batch2ProbeManager::ExecuteMicroBatch(
    std::vector<int>& failed_vars,
    std::vector<int>& failed_values) {
  failed_vars.clear();
  failed_values.clear();

  const int num_tasks = static_cast<int>(task_queue_.size());
  if (num_tasks == 0) {
    return 0;
  }

  CHECK(memory_allocated_) << "Batch2ProbeManager memory not allocated";

  // 保存一次快照，后续所有 probe 都在 workspace 私有域里跑
  SaveSnapshot();

  if (stats_enabled_) {
    last_probe_iterations_.assign(num_tasks, 0);
    last_probe_deletions_.assign(num_tasks, 0);
  } else {
    last_probe_iterations_.clear();
    last_probe_deletions_.clear();
  }

  int total_failed = 0;
  for (int offset = 0; offset < num_tasks; offset += max_batch_size_) {
    const int batch_size = std::min(max_batch_size_, num_tasks - offset);

    // 拷贝本批次任务到 device buffer
    cudaError_t err = cudaMemcpy(d_tasks_, task_queue_.data() + offset,
                                  batch_size * sizeof(ProbeTask),
                                  cudaMemcpyHostToDevice);
    CHECK(err == cudaSuccess) << "Failed to copy batch tasks to device: "
                              << cudaGetErrorString(err);

    // 启动 micro-batch kernel
    LaunchMicroBatchKernel(batch_size);

    // 收集统计（每个 workspace 对应一个 local_task_id）
    if (stats_enabled_) {
      for (int i = 0; i < batch_size; ++i) {
        last_probe_iterations_[offset + i] = d_workspaces_[i].iterations;
        last_probe_deletions_[offset + i] = d_workspaces_[i].deletions;
      }
    }

    // 收集结果
    total_failed += CollectBatchResults(
        batch_size, offset, failed_vars, failed_values);
  }

  // NOTE: Stage 1 kernel 当前不追踪 precheck 短路计数，返回 0
  // TODO: 如需精确统计，需修改 Stage 1 kernel 添加原子计数器
  last_precheck_short_circuit_count_ = 0;

  VLOG(1) << "Batch2 Micro-Batch completed: " << total_failed << " / "
          << num_tasks << " probes failed"
          << " (activation=" << (activation_strategy_ == 0 ? "FULL" : "NEIGHBOR")
          << ", precheck=" << (precheck_enabled_ ? "ON" : "OFF")
          << ", stats=" << (stats_enabled_ ? "ON" : "OFF") << ")";

  Clear();
  return total_failed;
}

// ============================================================================
// Batch-2 Stage 2: Persistent Blocks Manager
// ============================================================================

// 前向声明：Batch-2 Stage 2 wrapper（实现在 GModel.cu）
extern void LaunchBatch2PersistentBlocksKernelWrapper(
    GModelData model_data,
    Batch2PersistentControl* control);

// 计算最优 num_blocks 的静态方法
int Batch2PersistentManager::ComputeOptimalNumBlocks(int num_tasks,
                                                      int device_id) {
  if (num_tasks <= 0) return 8;  // 默认值

  cudaDeviceProp prop;
  cudaError_t err = cudaGetDeviceProperties(&prop, device_id);
  if (err != cudaSuccess) {
    LOG(WARNING) << "cudaGetDeviceProperties failed, using default num_blocks=8";
    return 8;
  }

  const int num_sms = prop.multiProcessorCount;  // Jetson Orin Nano: 8 SMs

  // 启发式规则：
  // 1. 如果任务数 <= 2*num_sms，用 num_sms 个 blocks（减少开销）
  // 2. 否则用 4*num_sms 个 blocks（增加并行度）
  // 3. 但不超过 num_tasks（避免空闲 blocks）
  int optimal;
  if (num_tasks <= 2 * num_sms) {
    optimal = num_sms;
  } else {
    optimal = 4 * num_sms;  // 32 blocks for Jetson Orin Nano
  }

  // 不超过任务数
  optimal = std::min(optimal, num_tasks);

  // 至少 1 个 block
  optimal = std::max(optimal, 1);

  VLOG(1) << "ComputeOptimalNumBlocks: num_tasks=" << num_tasks
          << ", num_sms=" << num_sms << ", optimal=" << optimal;

  return optimal;
}

Batch2PersistentManager::Batch2PersistentManager(GModel* model, int num_blocks)
    : model_(model),
      num_blocks_(num_blocks) {
  CHECK(model_ != nullptr) << "GModel pointer cannot be null";

  // num_blocks=-1 表示启用自适应调优
  if (num_blocks_ == -1) {
    auto_tune_enabled_ = true;
    // 使用默认值进行初始分配，后续会根据任务数重新调整
    num_blocks_ = 16;
    LOG(INFO) << "Batch2PersistentManager: auto-tune enabled, initial num_blocks=16";
  } else {
    CHECK(num_blocks_ > 0) << "num_blocks must be positive or -1 (auto)";
    auto_tune_enabled_ = false;
  }

  effective_num_blocks_ = num_blocks_;

  // 计算初始最大任务数（按 num_blocks * 4 预分配）
  max_tasks_ = num_blocks_ * 4;

  AllocateMemory();
}

Batch2PersistentManager::~Batch2PersistentManager() {
  FreeMemory();
}

void Batch2PersistentManager::AllocateMemory() {
  if (memory_allocated_) {
    return;
  }

  const int num_vars = model_->GetNumVars();
  const int bit_dom_int_size = model_->GetBitDomIntSize();
  const int num_cons = model_->GetNumCons();

  const int dom_words_per_world = num_vars * bit_dom_int_size;
  const int bitmap_size_words = (num_cons + 31) / 32;

  const size_t snapshot_bytes =
      static_cast<size_t>(dom_words_per_world) * sizeof(u32);
  const size_t dom_size_bytes = static_cast<size_t>(num_vars) * sizeof(int);
  const size_t tasks_bytes =
      static_cast<size_t>(max_tasks_) * sizeof(ProbeTask);
  const size_t results_bytes =
      static_cast<size_t>(max_tasks_) * sizeof(bool);

  // Workspaces（每个 block 一份）
  const size_t ws_struct_bytes =
      static_cast<size_t>(num_blocks_) * sizeof(WorldWorkspace);
  const size_t ws_bitdom_bytes =
      static_cast<size_t>(num_blocks_) *
      static_cast<size_t>(dom_words_per_world) * sizeof(u32);
  const size_t ws_dom_size_bytes =
      static_cast<size_t>(num_blocks_) * static_cast<size_t>(num_vars) *
      sizeof(int);
  const size_t ws_frontier_bytes =
      static_cast<size_t>(num_blocks_) *
      static_cast<size_t>(bitmap_size_words) * sizeof(u32);

  LOG(INFO) << "Allocating Batch2PersistentManager memory (Stage 2):";
  LOG(INFO) << "  num_blocks: " << num_blocks_;
  LOG(INFO) << "  max_tasks (initial): " << max_tasks_;
  LOG(INFO) << "  num_vars: " << num_vars;
  LOG(INFO) << "  num_cons: " << num_cons;
  LOG(INFO) << "  bit_dom_int_size: " << bit_dom_int_size;
  LOG(INFO) << "  bitmap_size_words: " << bitmap_size_words;

  cudaError_t err;

  // 任务数组
  err = cudaMallocManaged(&d_tasks_, tasks_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_tasks_: "
                            << cudaGetErrorString(err);

  // 结果数组
  err = cudaMallocManaged(&d_results_, results_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_results_: "
                            << cudaGetErrorString(err);

  // 域快照
  err = cudaMallocManaged(&d_snapshot_, snapshot_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_snapshot_: "
                            << cudaGetErrorString(err);

  // 域大小快照
  err = cudaMallocManaged(&d_dom_size_snapshot_, dom_size_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_dom_size_snapshot_: "
                            << cudaGetErrorString(err);

  // 全局任务游标
  err = cudaMallocManaged(&d_task_cursor_, sizeof(int));
  CHECK(err == cudaSuccess) << "Failed to allocate d_task_cursor_: "
                            << cudaGetErrorString(err);

  // 控制结构
  err = cudaMallocManaged(&d_control_, sizeof(Batch2PersistentControl));
  CHECK(err == cudaSuccess) << "Failed to allocate d_control_: "
                            << cudaGetErrorString(err);

  // Workspaces 结构体数组
  err = cudaMallocManaged(&d_workspaces_, ws_struct_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_workspaces_: "
                            << cudaGetErrorString(err);

  // Workspace 私有域
  err = cudaMallocManaged(&d_ws_bitdom_, ws_bitdom_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_ws_bitdom_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_ws_dom_size_, ws_dom_size_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_ws_dom_size_: "
                            << cudaGetErrorString(err);

  // Workspace 前沿位图
  err = cudaMallocManaged(&d_ws_frontier_A_, ws_frontier_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_ws_frontier_A_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_ws_frontier_B_, ws_frontier_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_ws_frontier_B_: "
                            << cudaGetErrorString(err);

  // Per-task 统计数组
  const size_t iterations_bytes =
      static_cast<size_t>(max_tasks_) * sizeof(int);
  const size_t deletions_bytes =
      static_cast<size_t>(max_tasks_) * sizeof(unsigned long long);

  err = cudaMallocManaged(&d_task_iterations_, iterations_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_task_iterations_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_task_deletions_, deletions_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_task_deletions_: "
                            << cudaGetErrorString(err);

  // P0-1 NEW: 三态状态数组
  const size_t status_bytes = static_cast<size_t>(max_tasks_) * sizeof(int8_t);
  err = cudaMallocManaged(&d_task_status_, status_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_task_status_: "
                            << cudaGetErrorString(err);

  // Precheck 短路计数器
  err = cudaMallocManaged(&d_precheck_short_circuit_count_,
                          sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_precheck_short_circuit_count_: "
                            << cudaGetErrorString(err);
  *d_precheck_short_circuit_count_ = 0;

  // 初始化每个 workspace 的切片指针
  for (int i = 0; i < num_blocks_; ++i) {
    WorldWorkspace& ws = d_workspaces_[i];
    ws.bitDom = d_ws_bitdom_ + static_cast<size_t>(i) * dom_words_per_world;
    ws.d_cur_dom_size = d_ws_dom_size_ + static_cast<size_t>(i) * num_vars;
    ws.frontier_A =
        d_ws_frontier_A_ + static_cast<size_t>(i) * bitmap_size_words;
    ws.frontier_B =
        d_ws_frontier_B_ + static_cast<size_t>(i) * bitmap_size_words;
    ws.inconsistent_flag = 0;
    ws.scanner_index = 0;
    ws.deletions = 0;
    ws.iterations = 0;
    ws.frontier_nonempty = 0;
  }

  memory_allocated_ = true;
  LOG(INFO) << "Batch2PersistentManager memory allocated successfully";
}

void Batch2PersistentManager::FreeMemory() {
  if (!memory_allocated_) {
    return;
  }

  if (d_tasks_) cudaFree(d_tasks_);
  if (d_results_) cudaFree(d_results_);
  if (d_snapshot_) cudaFree(d_snapshot_);
  if (d_dom_size_snapshot_) cudaFree(d_dom_size_snapshot_);
  if (d_task_cursor_) cudaFree(d_task_cursor_);
  if (d_control_) cudaFree(d_control_);
  if (d_workspaces_) cudaFree(d_workspaces_);
  if (d_ws_bitdom_) cudaFree(d_ws_bitdom_);
  if (d_ws_dom_size_) cudaFree(d_ws_dom_size_);
  if (d_ws_frontier_A_) cudaFree(d_ws_frontier_A_);
  if (d_ws_frontier_B_) cudaFree(d_ws_frontier_B_);
  if (d_task_iterations_) cudaFree(d_task_iterations_);
  if (d_task_deletions_) cudaFree(d_task_deletions_);
  if (d_task_status_) cudaFree(d_task_status_);  // P0-1 NEW
  if (d_precheck_short_circuit_count_) cudaFree(d_precheck_short_circuit_count_);

  d_tasks_ = nullptr;
  d_results_ = nullptr;
  d_snapshot_ = nullptr;
  d_dom_size_snapshot_ = nullptr;
  d_task_cursor_ = nullptr;
  d_control_ = nullptr;
  d_workspaces_ = nullptr;
  d_ws_bitdom_ = nullptr;
  d_ws_dom_size_ = nullptr;
  d_ws_frontier_A_ = nullptr;
  d_ws_frontier_B_ = nullptr;
  d_task_iterations_ = nullptr;
  d_task_deletions_ = nullptr;
  d_task_status_ = nullptr;  // P0-1 NEW
  d_precheck_short_circuit_count_ = nullptr;

  memory_allocated_ = false;
  LOG(INFO) << "Batch2PersistentManager memory freed";
}

void Batch2PersistentManager::AddTask(int var_id, int value) {
  const int num_vars = model_->GetNumVars();
  if (var_id < 0 || var_id >= num_vars) {
    LOG(ERROR) << "Invalid var_id=" << var_id
               << " (valid range: 0-" << (num_vars - 1) << ")";
    return;
  }

  task_queue_.emplace_back(var_id, value, task_queue_.size());
}

void Batch2PersistentManager::Clear() {
  task_queue_.clear();
}

void Batch2PersistentManager::SetActivationStrategy(int strategy) {
  CHECK(strategy == 0 || strategy == 1)
      << "Invalid activation strategy: " << strategy
      << " (must be 0=FULL or 1=NEIGHBOR)";
  activation_strategy_ = strategy;
}

void Batch2PersistentManager::EnablePrecheck(bool enabled) {
  precheck_enabled_ = enabled;
}

void Batch2PersistentManager::EnableStats(bool enabled) {
  stats_enabled_ = enabled;
}

void Batch2PersistentManager::ReserveTaskCapacity(int capacity) {
  if (capacity <= max_tasks_) {
    return;  // 已有足够容量
  }

  LOG(INFO) << "Batch2PersistentManager: pre-allocating task arrays: "
            << max_tasks_ << " -> " << capacity;

  // 释放旧的任务/结果/统计数组
  if (d_tasks_) cudaFree(d_tasks_);
  if (d_results_) cudaFree(d_results_);
  if (d_task_iterations_) cudaFree(d_task_iterations_);
  if (d_task_deletions_) cudaFree(d_task_deletions_);
  if (d_task_status_) cudaFree(d_task_status_);  // P0-1 NEW

  max_tasks_ = capacity;

  cudaError_t err;
  err = cudaMallocManaged(&d_tasks_, capacity * sizeof(ProbeTask));
  CHECK(err == cudaSuccess) << "Failed to reallocate d_tasks_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_results_, capacity * sizeof(bool));
  CHECK(err == cudaSuccess) << "Failed to reallocate d_results_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_task_iterations_, capacity * sizeof(int));
  CHECK(err == cudaSuccess) << "Failed to reallocate d_task_iterations_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_task_deletions_,
                          capacity * sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to reallocate d_task_deletions_: "
                            << cudaGetErrorString(err);

  // P0-1 NEW: 三态状态数组
  err = cudaMallocManaged(&d_task_status_, capacity * sizeof(int8_t));
  CHECK(err == cudaSuccess) << "Failed to reallocate d_task_status_: "
                            << cudaGetErrorString(err);
}

void Batch2PersistentManager::SaveSnapshot() {
  const int num_vars = model_->GetNumVars();
  const int bit_dom_int_size = model_->GetBitDomIntSize();
  const int snapshot_size_words = num_vars * bit_dom_int_size;

  const u32* current_domain = model_->GetBitDom();
  cudaError_t err = cudaMemcpy(d_snapshot_, current_domain,
                                snapshot_size_words * sizeof(u32),
                                cudaMemcpyDeviceToDevice);
  CHECK(err == cudaSuccess) << "Failed to save domain snapshot (bitDom): "
                            << cudaGetErrorString(err);

  const int* current_dom_sizes = model_->GetDomainSizesPtr();
  err = cudaMemcpy(d_dom_size_snapshot_, current_dom_sizes,
                    num_vars * sizeof(int),
                    cudaMemcpyDeviceToDevice);
  CHECK(err == cudaSuccess) << "Failed to save domain sizes snapshot: "
                            << cudaGetErrorString(err);

  err = cudaDeviceSynchronize();
  CHECK(err == cudaSuccess) << "cudaDeviceSynchronize failed after SaveSnapshot: "
                            << cudaGetErrorString(err);
}

void Batch2PersistentManager::LaunchPersistentBlocksKernel() {
  const int num_tasks = static_cast<int>(task_queue_.size());
  CHECK(num_tasks > 0) << "No tasks to execute";

  // 检查是否需要重新分配任务数组
  if (num_tasks > max_tasks_) {
    LOG(INFO) << "Reallocating task/result arrays: " << max_tasks_
              << " -> " << num_tasks;

    // 释放旧的任务/结果/统计数组
    if (d_tasks_) cudaFree(d_tasks_);
    if (d_results_) cudaFree(d_results_);
    if (d_task_iterations_) cudaFree(d_task_iterations_);
    if (d_task_deletions_) cudaFree(d_task_deletions_);

    max_tasks_ = num_tasks;

    cudaError_t err;
    err = cudaMallocManaged(&d_tasks_, num_tasks * sizeof(ProbeTask));
    CHECK(err == cudaSuccess) << "Failed to reallocate d_tasks_: "
                              << cudaGetErrorString(err);

    err = cudaMallocManaged(&d_results_, num_tasks * sizeof(bool));
    CHECK(err == cudaSuccess) << "Failed to reallocate d_results_: "
                              << cudaGetErrorString(err);

    err = cudaMallocManaged(&d_task_iterations_, num_tasks * sizeof(int));
    CHECK(err == cudaSuccess) << "Failed to reallocate d_task_iterations_: "
                              << cudaGetErrorString(err);

    err = cudaMallocManaged(&d_task_deletions_,
                            num_tasks * sizeof(unsigned long long));
    CHECK(err == cudaSuccess) << "Failed to reallocate d_task_deletions_: "
                              << cudaGetErrorString(err);
  }

  // 拷贝任务到 device buffer
  cudaError_t err = cudaMemcpy(d_tasks_, task_queue_.data(),
                                num_tasks * sizeof(ProbeTask),
                                cudaMemcpyHostToDevice);
  CHECK(err == cudaSuccess) << "Failed to copy tasks to device: "
                            << cudaGetErrorString(err);

  // 重置任务游标
  *d_task_cursor_ = 0;

  // 清空结果数组
  cudaMemset(d_results_, 0, num_tasks * sizeof(bool));

  // P0-1 NEW: 清空三态状态数组（默认 kOK = 0）
  if (d_task_status_) {
    cudaMemset(d_task_status_, 0, num_tasks * sizeof(int8_t));
  }

  // 初始化控制结构
  d_control_->num_tasks = num_tasks;
  d_control_->task_cursor = d_task_cursor_;
  d_control_->domain_snapshot = d_snapshot_;
  d_control_->dom_size_snapshot = d_dom_size_snapshot_;
  d_control_->tasks = d_tasks_;
  d_control_->results = d_results_;
  d_control_->task_status = d_task_status_;  // P0-1 NEW
  d_control_->task_iterations = stats_enabled_ ? d_task_iterations_ : nullptr;
  d_control_->task_deletions = stats_enabled_ ? d_task_deletions_ : nullptr;
  d_control_->workspaces = d_workspaces_;
  d_control_->num_blocks = effective_num_blocks_;
  d_control_->activation_strategy = activation_strategy_;
  d_control_->enable_precheck = precheck_enabled_ ? 1 : 0;
  d_control_->max_iterations_per_probe = max_iterations_per_probe_;  // P0-1 NEW: 使用成员变量
  d_control_->chunk_size = chunk_size_;

  // P0-1a NEW: 停滞检测参数
  d_control_->stagnation_threshold = stagnation_threshold_;
  d_control_->min_productivity = min_productivity_;
  d_control_->enable_stagnation_check = stagnation_check_enabled_ ? 1 : 0;

  // P0-1b NEW: 时间片调度参数
  d_control_->quantum_cid = quantum_cid_;
  d_control_->enable_quantum_check = quantum_check_enabled_ ? 1 : 0;

  // P0-2 NEW: NSAC allowed-constraints mask
  if (model_->IsNSACMaskEnabled() && model_->IsAllowedMasksBuilt()) {
    const auto& data = model_->GetGModelDataView();
    d_control_->allowed_masks = data.allowed_masks;
    d_control_->constraint_bitmap_words = data.constraint_bitmap_words;
  } else {
    d_control_->allowed_masks = nullptr;
    d_control_->constraint_bitmap_words = 0;
  }

  // Precheck 统计（如果 precheck 启用）
  if (precheck_enabled_ && d_precheck_short_circuit_count_) {
    *d_precheck_short_circuit_count_ = 0;  // 重置计数器
    d_control_->precheck_short_circuit_count = d_precheck_short_circuit_count_;
  } else {
    d_control_->precheck_short_circuit_count = nullptr;
  }

  // 同步确保控制结构写入完成
  err = cudaDeviceSynchronize();
  CHECK(err == cudaSuccess) << "cudaDeviceSynchronize failed before kernel: "
                            << cudaGetErrorString(err);

  // 获取 GModel 数据并启动 kernel
  GModelData model_data = model_->GetModelData();
  LaunchBatch2PersistentBlocksKernelWrapper(model_data, d_control_);

  // 等待 kernel 完成
  err = cudaDeviceSynchronize();
  CHECK(err == cudaSuccess) << "Persistent Blocks kernel failed: "
                            << cudaGetErrorString(err);
}

int Batch2PersistentManager::CollectResults(std::vector<int>& failed_vars,
                                            std::vector<int>& failed_values,
                                            std::vector<int>* unknown_vars,
                                            std::vector<int>* unknown_values) {
  const int num_tasks = static_cast<int>(task_queue_.size());
  int num_failed = 0;

  if (unknown_vars != nullptr) unknown_vars->clear();
  if (unknown_values != nullptr) unknown_values->clear();

  // P0-1 NEW: 重置三态统计
  last_statistics_.Reset();
  last_statistics_.total_probes = num_tasks;

  for (int i = 0; i < num_tasks; ++i) {
    // P0-1 NEW: 使用三态状态（如果可用）
    if (d_task_status_) {
      ProbeStatus status = static_cast<ProbeStatus>(d_task_status_[i]);
      switch (status) {
        case ProbeStatus::kDWO:
          // 仅对 DWO 删值
          failed_vars.push_back(task_queue_[i].var_id);
          failed_values.push_back(task_queue_[i].value);
          num_failed++;
          last_statistics_.dwo_count++;
          break;
        case ProbeStatus::kUNKNOWN:
          // 预算超限，不删值（保守处理）
          if (unknown_vars != nullptr) {
            unknown_vars->push_back(task_queue_[i].var_id);
          }
          if (unknown_values != nullptr) {
            unknown_values->push_back(task_queue_[i].value);
          }
          last_statistics_.unknown_count++;
          last_statistics_.budget_hit_count++;
          break;
        case ProbeStatus::kOK:
        default:
          // 正常收敛，不删值
          last_statistics_.ok_count++;
          break;
      }
    } else {
      // 回退到旧逻辑（兼容性）
      if (!d_results_[i]) {
        failed_vars.push_back(task_queue_[i].var_id);
        failed_values.push_back(task_queue_[i].value);
        num_failed++;
        last_statistics_.dwo_count++;
      } else {
        last_statistics_.ok_count++;
      }
    }
  }

  // 收集迭代统计（如果启用）
  if (stats_enabled_ && d_task_iterations_ && d_task_deletions_) {
    last_probe_iterations_.resize(num_tasks);
    last_probe_deletions_.resize(num_tasks);
    for (int i = 0; i < num_tasks; ++i) {
      last_probe_iterations_[i] = d_task_iterations_[i];
      last_probe_deletions_[i] = d_task_deletions_[i];

      // P0-1 NEW: 更新迭代统计
      int iters = d_task_iterations_[i];
      last_statistics_.total_iterations += iters;
      if (iters > last_statistics_.max_iterations) {
        last_statistics_.max_iterations = iters;
      }
      if (iters < last_statistics_.min_iterations) {
        last_statistics_.min_iterations = iters;
      }
    }
  }

  // 收集 precheck 短路统计
  if (precheck_enabled_ && d_precheck_short_circuit_count_) {
    last_precheck_short_circuit_count_ = *d_precheck_short_circuit_count_;
  } else {
    last_precheck_short_circuit_count_ = 0;
  }

  return num_failed;
}

int Batch2PersistentManager::ExecutePersistentBlocks(
    std::vector<int>& failed_vars,
    std::vector<int>& failed_values) {
  return ExecutePersistentBlocks(failed_vars, failed_values, nullptr, nullptr);
}

int Batch2PersistentManager::ExecutePersistentBlocks(
    std::vector<int>& failed_vars,
    std::vector<int>& failed_values,
    std::vector<int>* unknown_vars,
    std::vector<int>* unknown_values) {
  failed_vars.clear();
  failed_values.clear();
  if (unknown_vars != nullptr) unknown_vars->clear();
  if (unknown_values != nullptr) unknown_values->clear();

  const int num_tasks = static_cast<int>(task_queue_.size());
  if (num_tasks == 0) {
    return 0;
  }

  // 自适应 num_blocks 调优
  if (auto_tune_enabled_) {
    int optimal = ComputeOptimalNumBlocks(num_tasks, 0);
    if (optimal != effective_num_blocks_) {
      VLOG(1) << "Auto-tuning num_blocks: " << effective_num_blocks_
              << " -> " << optimal << " (num_tasks=" << num_tasks << ")";

      // 如果需要更多 blocks，需要重新分配 workspaces
      if (optimal > num_blocks_) {
        LOG(INFO) << "Reallocating workspaces for " << optimal << " blocks";
        FreeMemory();
        num_blocks_ = optimal;
        max_tasks_ = num_blocks_ * 4;
        AllocateMemory();
      }
      effective_num_blocks_ = optimal;
    }
  } else {
    effective_num_blocks_ = num_blocks_;
  }

  // 防御性：确保任务/结果数组容量足够（sac_benchmark 的 full_sac 会一次性提交全域任务）
  // 否则会导致 cudaMemcpy 越界写，进而在 kernel launch 时报 "invalid argument"。
  if (num_tasks > max_tasks_) {
    const int new_capacity = std::max(num_tasks, max_tasks_ * 2);
    ReserveTaskCapacity(new_capacity);
  }

  CHECK(memory_allocated_) << "Batch2PersistentManager memory not allocated";

  VLOG(1) << "ExecutePersistentBlocks: " << num_tasks << " tasks, "
          << effective_num_blocks_ << " blocks"
          << (auto_tune_enabled_ ? " (auto-tuned)" : "");

  // 保存快照
  SaveSnapshot();

  // 启动 persistent blocks kernel
  LaunchPersistentBlocksKernel();

  // 收集结果
  int num_failed = CollectResults(failed_vars, failed_values, unknown_vars, unknown_values);

  VLOG(1) << "Persistent Blocks completed: " << num_failed << " / "
          << num_tasks << " probes failed"
          << " (activation=" << (activation_strategy_ == 0 ? "FULL" : "NEIGHBOR")
          << ", blocks=" << effective_num_blocks_ << ")";

  Clear();
  return num_failed;
}

// ============================================================================
// AutoStageSelector 实现
// ============================================================================

AutoStageSelector::AutoStageSelector(GModel* model, int sample_batch_size)
    : model_(model), sample_batch_size_(sample_batch_size) {
  CHECK(model_ != nullptr) << "GModel cannot be null";
}

int AutoStageSelector::GetNumSMs() const {
  cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, 0);
  return prop.multiProcessorCount;
}

StageSelectionResult AutoStageSelector::DecideByTaskCount(int num_tasks) const {
  StageSelectionResult result;
  result.sampled = false;
  result.sample_fail_rate = 0;
  result.sample_avg_iterations = 0;
  result.sample_avg_deletions = 0;
  result.recommended_batch_size = 64;

  const int num_sms = GetNumSMs();
  // Jetson Orin Nano: 8 SMs
  // 策略：
  // - 小任务 (<= 2*num_sms=16)：Stage 1（开销更低）
  // - 中等任务 (17-500)：需要采样决定（fail_rate 是关键因素）
  // - 大任务 (>500)：可以直接选 Stage 2，但仍建议采样以验证

  const int small_threshold = 2 * num_sms;  // 16 for Jetson
  const int large_threshold = 500;          // 超过此值倾向 Stage 2

  if (num_tasks <= small_threshold) {
    // 小任务量：Stage 1 开销更低
    result.stage = StageSelection::kStage1;
    result.recommended_blocks = 0;
    result.reason = "num_tasks=" + std::to_string(num_tasks) +
                    " <= " + std::to_string(small_threshold) +
                    " -> Stage 1 (lower overhead)";
  } else if (num_tasks <= large_threshold) {
    // 中等任务量：需要采样决定
    // 关键因素是 fail_rate，不能仅凭 num_tasks 决定
    result.stage = StageSelection::kAuto;
    result.recommended_blocks = Batch2PersistentManager::ComputeOptimalNumBlocks(num_tasks, 0);
    result.reason = "num_tasks=" + std::to_string(num_tasks) +
                    " in range (" + std::to_string(small_threshold) +
                    ", " + std::to_string(large_threshold) +
                    "] -> need sampling to determine";
  } else {
    // 大任务量：默认 Stage 2，但仍建议采样以优化
    result.stage = StageSelection::kAuto;
    result.recommended_blocks = Batch2PersistentManager::ComputeOptimalNumBlocks(num_tasks, 0);
    result.reason = "num_tasks=" + std::to_string(num_tasks) +
                    " > " + std::to_string(large_threshold) +
                    " -> likely Stage 2, sampling to confirm";
  }

  return result;
}

StageSelectionResult AutoStageSelector::DecideWithSampling(
    const std::vector<ProbeTask>& tasks) {
  const int num_tasks = static_cast<int>(tasks.size());

  // 首先检查任务数量
  StageSelectionResult result = DecideByTaskCount(num_tasks);

  // 如果任务数量已经决定了，或者任务太少无法采样，直接返回
  if (result.stage != StageSelection::kAuto ||
      num_tasks < sample_batch_size_) {
    LOG(INFO) << "[AutoStageSelector] Decision without sampling: " << result.reason;
    return result;
  }

  // 执行采样：使用 Stage 1 运行一小批任务
  LOG(INFO) << "[AutoStageSelector] Sampling " << sample_batch_size_
            << " probes out of " << num_tasks << "...";

  Batch2ProbeManager sampler(model_, sample_batch_size_);
  sampler.SetActivationStrategy(1);  // NEIGHBOR
  sampler.EnableStats(true);

  // 添加采样任务（从头开始取）
  for (int i = 0; i < sample_batch_size_ && i < num_tasks; ++i) {
    sampler.AddTask(tasks[i].var_id, tasks[i].value);
  }

  std::vector<int> failed_vars, failed_values;
  sampler.ExecuteMicroBatch(failed_vars, failed_values);

  // 收集采样统计
  const auto& iterations = sampler.GetLastProbeIterations();
  const auto& deletions = sampler.GetLastProbeDeletions();

  int num_failed = static_cast<int>(failed_vars.size());
  result.sample_fail_rate = static_cast<double>(num_failed) / sample_batch_size_;

  double total_iter = 0, total_del = 0;
  for (int i = 0; i < static_cast<int>(iterations.size()); ++i) {
    total_iter += iterations[i];
    total_del += deletions[i];
  }
  result.sample_avg_iterations = iterations.empty() ? 0 : total_iter / iterations.size();
  result.sample_avg_deletions = deletions.empty() ? 0 : total_del / deletions.size();
  result.sampled = true;

  // ========== 计算分布统计 ==========
  // CV (Coefficient of Variation) = stddev / mean
  // P95/P50 ratio 检测长尾分布
  double cv_iter = 0.0, cv_del = 0.0;
  double p95_p50_iter = 1.0, p95_p50_del = 1.0;

  if (!iterations.empty() && result.sample_avg_iterations > 0) {
    // 计算 iterations 的标准差
    double sum_sq_diff = 0;
    for (int i = 0; i < static_cast<int>(iterations.size()); ++i) {
      double diff = iterations[i] - result.sample_avg_iterations;
      sum_sq_diff += diff * diff;
    }
    double stddev_iter = std::sqrt(sum_sq_diff / iterations.size());
    cv_iter = stddev_iter / result.sample_avg_iterations;

    // 计算 P95/P50 ratio（排序后取分位数）
    std::vector<int> sorted_iter = iterations;
    std::sort(sorted_iter.begin(), sorted_iter.end());
    int p50_idx = static_cast<int>(sorted_iter.size() * 0.5);
    int p95_idx = static_cast<int>(sorted_iter.size() * 0.95);
    p95_idx = std::min(p95_idx, static_cast<int>(sorted_iter.size()) - 1);
    int p50_iter = sorted_iter[p50_idx];
    int p95_iter = sorted_iter[p95_idx];
    if (p50_iter > 0) {
      p95_p50_iter = static_cast<double>(p95_iter) / p50_iter;
    }
  }

  if (!deletions.empty() && result.sample_avg_deletions > 0) {
    // 计算 deletions 的标准差
    double sum_sq_diff = 0;
    for (int i = 0; i < static_cast<int>(deletions.size()); ++i) {
      double diff = static_cast<double>(deletions[i]) - result.sample_avg_deletions;
      sum_sq_diff += diff * diff;
    }
    double stddev_del = std::sqrt(sum_sq_diff / deletions.size());
    cv_del = stddev_del / result.sample_avg_deletions;

    // 计算 P95/P50 ratio
    std::vector<unsigned long long> sorted_del = deletions;
    std::sort(sorted_del.begin(), sorted_del.end());
    int p50_idx = static_cast<int>(sorted_del.size() * 0.5);
    int p95_idx = static_cast<int>(sorted_del.size() * 0.95);
    p95_idx = std::min(p95_idx, static_cast<int>(sorted_del.size()) - 1);
    unsigned long long p50_del = sorted_del[p50_idx];
    unsigned long long p95_del = sorted_del[p95_idx];
    if (p50_del > 0) {
      p95_p50_del = static_cast<double>(p95_del) / p50_del;
    }
  }

  // 取两者中更大的 CV 和 P95/P50
  double max_cv = std::max(cv_iter, cv_del);
  double max_p95_p50 = std::max(p95_p50_iter, p95_p50_del);

  LOG(INFO) << "[AutoStageSelector] Distribution stats: CV_iter=" << std::fixed
            << std::setprecision(2) << cv_iter << ", CV_del=" << cv_del
            << ", P95/P50_iter=" << p95_p50_iter << ", P95/P50_del=" << p95_p50_del;

  // ========== 基于采样结果决策 ==========
  // 关键洞察：
  // - 高失败率 (fail_rate > 30%) → Stage 2 优势明显（任务窃取高效）
  // - 低失败率 + 均匀收敛（低 CV、低 P95/P50）→ Stage 1 优势（固定开销低）
  // - 高 CV (>0.5) 或高 P95/P50 (>2.0) → 工作不均衡 → Stage 2
  // - 移除了 num_tasks > 200 的硬触发，改用分布判据

  // 阈值定义
  const double high_cv_threshold = 0.5;        // CV > 0.5 表示分布不均匀
  const double high_p95_p50_threshold = 2.0;   // P95/P50 > 2.0 表示长尾分布

  bool favor_stage2 = false;
  std::string decision_reason;

  // ========== 决策逻辑 ==========
  // 核心原则：只有在任务负载不均衡时才选 Stage 2
  // - Stage 2 的优势是动态任务分配，适合不均衡场景
  // - Stage 1 的优势是低开销，适合均衡场景
  //
  // 不均衡的证据：
  // 1. 高失败率（fail_rate > 30%）→ 不一致任务的早终止导致不均衡
  // 2. 高 CV (>0.5) 或高 P95/P50 (>2.0) → 工作量分布不均匀
  //
  // 高删值量本身不是选择 Stage 2 的理由，因为：
  // - 如果所有任务删值都高（均匀），Stage 1 开销更低
  // - 只有删值量不均匀时，Stage 2 的任务窃取才有优势

  // 检测不均衡：高 CV 或 高 P95/P50
  bool is_unbalanced = (max_cv > high_cv_threshold) || (max_p95_p50 > high_p95_p50_threshold);

  // 主要判据：高失败率 → Stage 2（失败任务早终止导致不均衡）
  if (result.sample_fail_rate >= high_fail_rate_threshold_) {
    favor_stage2 = true;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    oss << "high fail_rate=" << (result.sample_fail_rate * 100) << "%";
    decision_reason = oss.str();
  }
  // 次要判据：分布不均衡（高 CV 或高 P95/P50）→ Stage 2 任务窃取更高效
  else if (is_unbalanced) {
    favor_stage2 = true;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    if (max_cv > high_cv_threshold) {
      oss << "unbalanced CV=" << max_cv << " (>" << high_cv_threshold << ")";
    } else {
      oss << "long-tail P95/P50=" << max_p95_p50 << " (>" << high_p95_p50_threshold << ")";
    }
    decision_reason = oss.str();
  }
  // 均匀分布 → Stage 1（即使删值量高或任务数大）
  else {
    favor_stage2 = false;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    oss << "uniform: fail_rate=" << (result.sample_fail_rate * 100) << "%, "
        << "CV=" << std::setprecision(2) << max_cv << ", "
        << "P95/P50=" << max_p95_p50 << ", "
        << "avg_del=" << std::setprecision(1) << result.sample_avg_deletions;
    decision_reason = oss.str();
  }

  if (favor_stage2) {
    result.stage = StageSelection::kStage2;
    result.recommended_blocks = Batch2PersistentManager::ComputeOptimalNumBlocks(num_tasks, 0);
    result.reason = "Sampling: " + decision_reason + " -> Stage 2 (blocks=" +
                    std::to_string(result.recommended_blocks) + ")";
  } else {
    result.stage = StageSelection::kStage1;
    result.recommended_blocks = 0;
    result.reason = "Sampling: " + decision_reason + " -> Stage 1";
  }

  LOG(INFO) << "[AutoStageSelector] " << result.reason;
  LOG(INFO) << "[AutoStageSelector] Sample stats: fail_rate="
            << std::fixed << std::setprecision(1) << (result.sample_fail_rate * 100)
            << "%, avg_iter=" << result.sample_avg_iterations
            << ", avg_del=" << result.sample_avg_deletions;

  return result;
}

// ============================================================================
// DecideWithTimedComparison - 实测对比选择（更准确）
// ============================================================================
StageSelectionResult AutoStageSelector::DecideWithTimedComparison(
    const std::vector<ProbeTask>& tasks, int num_blocks) {
  const int num_tasks = static_cast<int>(tasks.size());

  StageSelectionResult result;
  result.sampled = false;
  result.timed = false;
  result.sample_fail_rate = 0;
  result.sample_avg_iterations = 0;
  result.sample_avg_deletions = 0;
  result.stage1_time_ms = 0;
  result.stage2_time_ms = 0;
  result.speedup_ratio = 1.0;
  result.recommended_batch_size = 64;

  // 小任务量直接选 Stage 1
  const int num_sms = GetNumSMs();
  const int small_threshold = 2 * num_sms;
  if (num_tasks <= small_threshold) {
    result.stage = StageSelection::kStage1;
    result.recommended_blocks = 0;
    result.reason = "num_tasks=" + std::to_string(num_tasks) +
                    " <= " + std::to_string(small_threshold) +
                    " -> Stage 1 (small task count)";
    LOG(INFO) << "[AutoStageSelector] " << result.reason;
    return result;
  }

  // 任务太少无法采样
  if (num_tasks < sample_batch_size_) {
    result.stage = StageSelection::kStage1;
    result.recommended_blocks = 0;
    result.reason = "num_tasks=" + std::to_string(num_tasks) +
                    " < sample_size=" + std::to_string(sample_batch_size_) +
                    " -> Stage 1 (insufficient for sampling)";
    LOG(INFO) << "[AutoStageSelector] " << result.reason;
    return result;
  }

  LOG(INFO) << "[AutoStageSelector] Timed comparison with " << sample_batch_size_
            << " probes out of " << num_tasks << "...";

  // 准备采样任务
  std::vector<ProbeTask> sample_tasks;
  for (int i = 0; i < sample_batch_size_ && i < num_tasks; ++i) {
    sample_tasks.push_back(tasks[i]);
  }

  // 计算 Stage 2 blocks
  int stage2_blocks = (num_blocks == -1)
      ? Batch2PersistentManager::ComputeOptimalNumBlocks(num_tasks, 0)
      : num_blocks;
  result.recommended_blocks = stage2_blocks;

  // ========== Stage 1 实测 ==========
  {
    Batch2ProbeManager stage1(model_, sample_batch_size_);
    stage1.SetActivationStrategy(1);  // NEIGHBOR
    stage1.EnableStats(true);

    for (const auto& t : sample_tasks) {
      stage1.AddTask(t.var_id, t.value);
    }

    std::vector<int> failed_vars, failed_values;

    // 预热
    stage1.ExecuteMicroBatch(failed_vars, failed_values);

    // 重新添加任务
    for (const auto& t : sample_tasks) {
      stage1.AddTask(t.var_id, t.value);
    }
    failed_vars.clear();
    failed_values.clear();

    // 计时
    cudaDeviceSynchronize();
    auto start = std::chrono::high_resolution_clock::now();
    stage1.ExecuteMicroBatch(failed_vars, failed_values);
    cudaDeviceSynchronize();
    auto end = std::chrono::high_resolution_clock::now();

    result.stage1_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    // 收集统计
    int num_failed = static_cast<int>(failed_vars.size());
    result.sample_fail_rate = static_cast<double>(num_failed) / sample_batch_size_;

    const auto& iterations = stage1.GetLastProbeIterations();
    const auto& deletions = stage1.GetLastProbeDeletions();
    double total_iter = 0, total_del = 0;
    for (size_t i = 0; i < iterations.size(); ++i) {
      total_iter += iterations[i];
      total_del += deletions[i];
    }
    result.sample_avg_iterations = iterations.empty() ? 0 : total_iter / iterations.size();
    result.sample_avg_deletions = deletions.empty() ? 0 : total_del / deletions.size();
  }

  // ========== Stage 2 实测 ==========
  {
    // 使用固定 num_blocks，避免 auto-tune 重分配开销
    Batch2PersistentManager stage2(model_, stage2_blocks);
    stage2.SetActivationStrategy(1);  // NEIGHBOR
    stage2.EnableStats(false);  // 已有 Stage 1 统计，不需要重复

    for (const auto& t : sample_tasks) {
      stage2.AddTask(t.var_id, t.value);
    }

    std::vector<int> failed_vars, failed_values;

    // 预热
    stage2.ExecutePersistentBlocks(failed_vars, failed_values);

    // 重新添加任务
    for (const auto& t : sample_tasks) {
      stage2.AddTask(t.var_id, t.value);
    }
    failed_vars.clear();
    failed_values.clear();

    // 计时
    cudaDeviceSynchronize();
    auto start = std::chrono::high_resolution_clock::now();
    stage2.ExecutePersistentBlocks(failed_vars, failed_values);
    cudaDeviceSynchronize();
    auto end = std::chrono::high_resolution_clock::now();

    result.stage2_time_ms = std::chrono::duration<double, std::milli>(end - start).count();
  }

  result.sampled = true;
  result.timed = true;

  // 计算加速比
  if (result.stage1_time_ms > 0) {
    result.speedup_ratio = result.stage1_time_ms / result.stage2_time_ms;
  }

  // 基于实测时间选择
  // 使用 10% 的容差，如果差距在 10% 以内则使用统计作为 tiebreaker
  const double tolerance = 0.10;
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2);

  // 特殊处理：高失败率（>50%）+ 大任务量 → 倾向 Stage 2
  // 原因：小采样无法体现 Stage 2 在大规模高失败率场景的动态调度优势
  const double very_high_fail_rate = 0.50;
  const int large_task_threshold = 500;

  if (result.sample_fail_rate >= very_high_fail_rate && num_tasks >= large_task_threshold) {
    // 高失败率 + 大任务量：覆盖实测结果，选择 Stage 2
    result.stage = StageSelection::kStage2;
    oss << "Override: high fail_rate=" << std::setprecision(0)
        << (result.sample_fail_rate * 100) << "% + large num_tasks="
        << num_tasks << " -> Stage 2 (dynamic scheduling advantage)";
    result.reason = oss.str();
  } else if (result.speedup_ratio >= (1.0 + tolerance)) {
    // Stage 2 明显更快
    result.stage = StageSelection::kStage2;
    oss << "Timed: Stage 2 faster by " << result.speedup_ratio << "x "
        << "(S1=" << std::setprecision(1) << result.stage1_time_ms << "ms, "
        << "S2=" << result.stage2_time_ms << "ms)";
    result.reason = oss.str();
  } else if (result.speedup_ratio <= (1.0 - tolerance)) {
    // Stage 1 明显更快
    result.stage = StageSelection::kStage1;
    oss << "Timed: Stage 1 faster by " << (1.0 / result.speedup_ratio) << "x "
        << "(S1=" << std::setprecision(1) << result.stage1_time_ms << "ms, "
        << "S2=" << result.stage2_time_ms << "ms)";
    result.reason = oss.str();
  } else {
    // 差距在容差内，使用 fail_rate 作为 tiebreaker
    if (result.sample_fail_rate >= high_fail_rate_threshold_) {
      result.stage = StageSelection::kStage2;
      oss << "Timed: ~equal (" << result.speedup_ratio << "x), "
          << "tie-break by high fail_rate=" << std::setprecision(0)
          << (result.sample_fail_rate * 100) << "% -> Stage 2";
    } else {
      result.stage = StageSelection::kStage1;
      oss << "Timed: ~equal (" << result.speedup_ratio << "x), "
          << "tie-break by low fail_rate -> Stage 1";
    }
    result.reason = oss.str();
  }

  LOG(INFO) << "[AutoStageSelector] " << result.reason;
  LOG(INFO) << "[AutoStageSelector] Sample stats: fail_rate="
            << std::fixed << std::setprecision(1) << (result.sample_fail_rate * 100)
            << "%, avg_iter=" << result.sample_avg_iterations
            << ", avg_del=" << result.sample_avg_deletions;

  return result;
}

// ============================================================================
// DecideCached - 生产用法：带缓存的决策
// ============================================================================
StageSelectionResult AutoStageSelector::DecideCached(
    const std::vector<ProbeTask>& tasks, int num_blocks) {
  const int num_tasks = static_cast<int>(tasks.size());

  // 先用“任务量快速决策”兜底：小任务直接选 Stage 1（避免被大任务的缓存污染）
  StageSelectionResult quick = DecideByTaskCount(num_tasks);
  if (quick.stage == StageSelection::kStage1) {
    return quick;
  }

  // Bucketed cache：
  // - 中等任务量（<=500）：一份缓存
  // - 大任务量（>500）：一份缓存
  //
  // 目的：避免 “第一次采样是大 batch → 后续小/中 batch 也被迫 Stage 2” 或反之。
  static constexpr int kLargeTaskThreshold = 500;
  const bool is_large = num_tasks > kLargeTaskThreshold;

  bool* cache_valid = is_large ? &cache_large_valid_ : &cache_medium_valid_;
  StageSelectionResult* cached = is_large ? &cached_large_result_ : &cached_medium_result_;

  if (*cache_valid) {
    LOG(INFO) << "[AutoStageSelector] Using cached decision (bucket="
              << (is_large ? "large" : "medium") << "): "
              << (cached->stage == StageSelection::kStage1 ? "Stage 1" : "Stage 2")
              << " (reason: " << cached->reason << ")";
    return *cached;
  }

  LOG(INFO) << "[AutoStageSelector] No cache (bucket="
            << (is_large ? "large" : "medium")
            << "), running timed comparison...";
  *cached = DecideWithTimedComparison(tasks, num_blocks);
  *cache_valid = true;
  LOG(INFO) << "[AutoStageSelector] Decision cached (bucket="
            << (is_large ? "large" : "medium") << ")";
  return *cached;
}

// ============================================================================
// Batch-3A: 约束聚合管理器实现
// ============================================================================

// 前向声明：Batch-3A kernel wrapper（实现在 GModel.cu）
extern void LaunchBatch3AKernelWrapper(
    GModelData model_data,
    Batch3AControl* control);

Batch3AManager::Batch3AManager(GModel* model, int num_blocks, int max_worlds)
    : model_(model),
      num_blocks_(num_blocks),
      max_worlds_(max_worlds) {
  CHECK(model_ != nullptr) << "GModel pointer cannot be null";
  CHECK(max_worlds_ > 0 && max_worlds_ <= 32)
      << "max_worlds must be in [1, 32], got " << max_worlds_;

  // MVP: 固定使用单 block，避免跨 block 同步问题
  // 后续 warp-per-world 优化完成后可改为多 block
  if (num_blocks_ == -1) {
    num_blocks_ = 1;  // MVP: 单 block
    LOG(INFO) << "Batch3AManager: MVP mode, num_blocks=" << num_blocks_;
  } else {
    // 显式指定时也强制为 1（MVP 限制）
    if (num_blocks_ != 1) {
      LOG(WARNING) << "Batch3AManager: MVP mode forces num_blocks=1 (requested "
                   << num_blocks_ << ")";
      num_blocks_ = 1;
    }
  }

  AllocateMemory();
}

Batch3AManager::~Batch3AManager() {
  FreeMemory();
}

void Batch3AManager::AllocateMemory() {
  if (memory_allocated_) {
    return;
  }

  const int num_vars = model_->GetNumVars();
  const int bit_dom_int_size = model_->GetBitDomIntSize();
  const int num_cons = model_->GetNumCons();

  const int dom_words_per_world = num_vars * bit_dom_int_size;
  const int bitmap_size_words = (num_cons + 31) / 32;

  LOG(INFO) << "Allocating Batch3AManager memory:";
  LOG(INFO) << "  max_worlds: " << max_worlds_;
  LOG(INFO) << "  num_blocks: " << num_blocks_;
  LOG(INFO) << "  num_vars: " << num_vars;
  LOG(INFO) << "  num_cons: " << num_cons;
  LOG(INFO) << "  bit_dom_int_size: " << bit_dom_int_size;
  LOG(INFO) << "  bitmap_size_words: " << bitmap_size_words;

  cudaError_t err;

  // ========== 任务与结果 ==========
  err = cudaMallocManaged(&d_tasks_, max_worlds_ * sizeof(ProbeTask));
  CHECK(err == cudaSuccess) << "Failed to allocate d_tasks_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_results_, max_worlds_ * sizeof(bool));
  CHECK(err == cudaSuccess) << "Failed to allocate d_results_: "
                            << cudaGetErrorString(err);

  // 约束任务数组（最大 num_cons 个约束）
  err = cudaMallocManaged(&d_constraint_tasks_, num_cons * sizeof(Batch3ATask));
  CHECK(err == cudaSuccess) << "Failed to allocate d_constraint_tasks_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_constraint_task_cursor_, sizeof(int));
  CHECK(err == cudaSuccess) << "Failed to allocate d_constraint_task_cursor_: "
                            << cudaGetErrorString(err);

  // ========== 快照 ==========
  const size_t snapshot_bytes = dom_words_per_world * sizeof(u32);
  const size_t dom_size_bytes = num_vars * sizeof(int);

  err = cudaMallocManaged(&d_snapshot_, snapshot_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_snapshot_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_dom_size_snapshot_, dom_size_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_dom_size_snapshot_: "
                            << cudaGetErrorString(err);

  // ========== Workspaces（每个 world 一份）==========
  err = cudaMallocManaged(&d_workspaces_, max_worlds_ * sizeof(WorldWorkspace));
  CHECK(err == cudaSuccess) << "Failed to allocate d_workspaces_: "
                            << cudaGetErrorString(err);

  const size_t ws_bitdom_bytes =
      static_cast<size_t>(max_worlds_) * dom_words_per_world * sizeof(u32);
  const size_t ws_dom_size_bytes =
      static_cast<size_t>(max_worlds_) * num_vars * sizeof(int);
  const size_t ws_frontier_bytes =
      static_cast<size_t>(max_worlds_) * bitmap_size_words * sizeof(u32);

  err = cudaMallocManaged(&d_ws_bitdom_, ws_bitdom_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_ws_bitdom_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_ws_dom_size_, ws_dom_size_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_ws_dom_size_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_ws_frontier_A_, ws_frontier_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_ws_frontier_A_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_ws_frontier_B_, ws_frontier_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_ws_frontier_B_: "
                            << cudaGetErrorString(err);

  // ========== Dynamic Submission 队列（每个 block 一套）==========
  // 说明：
  // - 该内存用于替代 Batch-3A kernel 内单线程全量扫描构建任务；
  // - 以 `<cid, world_mask>` 的形式在 device 侧动态提交下一轮要检查的约束；
  // - mask/queue 维度与约束数成正比，避免每轮扫 `num_cons`。
  queue_capacity_ = num_cons;
  const size_t mask_bytes =
      static_cast<size_t>(kMaxBatch3ABlocks) * num_cons * sizeof(u32);
  const size_t queue_bytes =
      static_cast<size_t>(kMaxBatch3ABlocks) * queue_capacity_ * sizeof(int);

  err = cudaMallocManaged(&d_block_frontier_mask_A_, mask_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_block_frontier_mask_A_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_block_frontier_mask_B_, mask_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_block_frontier_mask_B_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_block_cid_queue_A_, queue_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_block_cid_queue_A_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_block_cid_queue_B_, queue_bytes);
  CHECK(err == cudaSuccess) << "Failed to allocate d_block_cid_queue_B_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_block_queue_tail_A_, kMaxBatch3ABlocks * sizeof(int));
  CHECK(err == cudaSuccess) << "Failed to allocate d_block_queue_tail_A_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_block_queue_tail_B_, kMaxBatch3ABlocks * sizeof(int));
  CHECK(err == cudaSuccess) << "Failed to allocate d_block_queue_tail_B_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_block_overflow_, kMaxBatch3ABlocks * sizeof(int));
  CHECK(err == cudaSuccess) << "Failed to allocate d_block_overflow_: "
                            << cudaGetErrorString(err);

  // 初始化每个 workspace 的切片指针
  for (int i = 0; i < max_worlds_; ++i) {
    WorldWorkspace& ws = d_workspaces_[i];
    ws.bitDom = d_ws_bitdom_ + static_cast<size_t>(i) * dom_words_per_world;
    ws.d_cur_dom_size = d_ws_dom_size_ + static_cast<size_t>(i) * num_vars;
    ws.frontier_A =
        d_ws_frontier_A_ + static_cast<size_t>(i) * bitmap_size_words;
    ws.frontier_B =
        d_ws_frontier_B_ + static_cast<size_t>(i) * bitmap_size_words;
    ws.inconsistent_flag = 0;
    ws.scanner_index = 0;
    ws.deletions = 0;
    ws.iterations = 0;
    ws.frontier_nonempty = 0;
  }

  // ========== 控制结构 ==========
  err = cudaMallocManaged(&d_control_, sizeof(Batch3AControl));
  CHECK(err == cudaSuccess) << "Failed to allocate d_control_: "
                            << cudaGetErrorString(err);

  // ========== 全局迭代控制 ==========
  err = cudaMallocManaged(&d_global_iteration_, sizeof(int));
  CHECK(err == cudaSuccess) << "Failed to allocate d_global_iteration_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_all_converged_flag_, sizeof(int));
  CHECK(err == cudaSuccess) << "Failed to allocate d_all_converged_flag_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_any_world_active_, sizeof(int));
  CHECK(err == cudaSuccess) << "Failed to allocate d_any_world_active_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_active_world_mask_, sizeof(u32));
  CHECK(err == cudaSuccess) << "Failed to allocate d_active_world_mask_: "
                            << cudaGetErrorString(err);

  // ========== 统计 ==========
  err = cudaMallocManaged(&d_total_constraint_checks_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_total_constraint_checks_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_total_deletions_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_total_deletions_: "
                            << cudaGetErrorString(err);

  memory_allocated_ = true;
  LOG(INFO) << "Batch3AManager memory allocated successfully";
}

void Batch3AManager::FreeMemory() {
  if (!memory_allocated_) {
    return;
  }

  if (d_tasks_) cudaFree(d_tasks_);
  if (d_results_) cudaFree(d_results_);
  if (d_constraint_tasks_) cudaFree(d_constraint_tasks_);
  if (d_constraint_task_cursor_) cudaFree(d_constraint_task_cursor_);
  if (d_snapshot_) cudaFree(d_snapshot_);
  if (d_dom_size_snapshot_) cudaFree(d_dom_size_snapshot_);
  if (d_workspaces_) cudaFree(d_workspaces_);
  if (d_ws_bitdom_) cudaFree(d_ws_bitdom_);
  if (d_ws_dom_size_) cudaFree(d_ws_dom_size_);
  if (d_ws_frontier_A_) cudaFree(d_ws_frontier_A_);
  if (d_ws_frontier_B_) cudaFree(d_ws_frontier_B_);
  if (d_block_frontier_mask_A_) cudaFree(d_block_frontier_mask_A_);
  if (d_block_frontier_mask_B_) cudaFree(d_block_frontier_mask_B_);
  if (d_block_cid_queue_A_) cudaFree(d_block_cid_queue_A_);
  if (d_block_cid_queue_B_) cudaFree(d_block_cid_queue_B_);
  if (d_block_queue_tail_A_) cudaFree(d_block_queue_tail_A_);
  if (d_block_queue_tail_B_) cudaFree(d_block_queue_tail_B_);
  if (d_block_overflow_) cudaFree(d_block_overflow_);
  if (d_control_) cudaFree(d_control_);
  if (d_global_iteration_) cudaFree(d_global_iteration_);
  if (d_all_converged_flag_) cudaFree(d_all_converged_flag_);
  if (d_any_world_active_) cudaFree(d_any_world_active_);
  if (d_active_world_mask_) cudaFree(d_active_world_mask_);
  if (d_total_constraint_checks_) cudaFree(d_total_constraint_checks_);
  if (d_total_deletions_) cudaFree(d_total_deletions_);

  d_tasks_ = nullptr;
  d_results_ = nullptr;
  d_constraint_tasks_ = nullptr;
  d_constraint_task_cursor_ = nullptr;
  d_snapshot_ = nullptr;
  d_dom_size_snapshot_ = nullptr;
  d_workspaces_ = nullptr;
  d_ws_bitdom_ = nullptr;
  d_ws_dom_size_ = nullptr;
  d_ws_frontier_A_ = nullptr;
  d_ws_frontier_B_ = nullptr;
  d_block_frontier_mask_A_ = nullptr;
  d_block_frontier_mask_B_ = nullptr;
  d_block_cid_queue_A_ = nullptr;
  d_block_cid_queue_B_ = nullptr;
  d_block_queue_tail_A_ = nullptr;
  d_block_queue_tail_B_ = nullptr;
  d_block_overflow_ = nullptr;
  d_control_ = nullptr;
  d_global_iteration_ = nullptr;
  d_all_converged_flag_ = nullptr;
  d_any_world_active_ = nullptr;
  d_active_world_mask_ = nullptr;
  d_total_constraint_checks_ = nullptr;
  d_total_deletions_ = nullptr;

  memory_allocated_ = false;
  LOG(INFO) << "Batch3AManager memory freed";
}

void Batch3AManager::AddTask(int var_id, int value) {
  const int num_vars = model_->GetNumVars();
  if (var_id < 0 || var_id >= num_vars) {
    LOG(ERROR) << "Invalid var_id=" << var_id
               << " (valid range: 0-" << (num_vars - 1) << ")";
    return;
  }

  task_queue_.emplace_back(var_id, value, task_queue_.size());
}

void Batch3AManager::Clear() {
  task_queue_.clear();
}

void Batch3AManager::SaveSnapshot() {
  const int num_vars = model_->GetNumVars();
  const int bit_dom_int_size = model_->GetBitDomIntSize();
  const int snapshot_size_words = num_vars * bit_dom_int_size;

  const u32* current_domain = model_->GetBitDom();
  cudaError_t err = cudaMemcpy(d_snapshot_, current_domain,
                                snapshot_size_words * sizeof(u32),
                                cudaMemcpyDeviceToDevice);
  CHECK(err == cudaSuccess) << "Failed to save domain snapshot (bitDom): "
                            << cudaGetErrorString(err);

  const int* current_dom_sizes = model_->GetDomainSizesPtr();
  err = cudaMemcpy(d_dom_size_snapshot_, current_dom_sizes,
                    num_vars * sizeof(int),
                    cudaMemcpyDeviceToDevice);
  CHECK(err == cudaSuccess) << "Failed to save domain sizes snapshot: "
                            << cudaGetErrorString(err);

  err = cudaDeviceSynchronize();
  CHECK(err == cudaSuccess) << "cudaDeviceSynchronize failed after SaveSnapshot: "
                            << cudaGetErrorString(err);
}

void Batch3AManager::BuildConstraintTasks(int num_worlds) {
  // 构建约束聚合任务：
  // 对于每个约束，检查哪些 world 需要检查该约束（frontier 中有该约束）
  //
  // MVP 版本简化：初始时所有 world 的 frontier 相同（只包含 probe 变量的邻域约束）
  // 但不同 probe 可能有不同的变量，所以需要合并所有 probe 涉及的约束
  //
  // 实际策略：在 kernel 内部动态构建，这里只准备最大容量
  // 因为每轮迭代的活跃约束不同，需要在 kernel 内根据 frontier 动态构建

  const int num_cons = model_->GetNumCons();

  // 初始化：所有约束都可能被激活，world_mask 在 kernel 内动态更新
  // 这里先设置最大可能的任务数
  d_control_->num_constraint_tasks = num_cons;

  // 重置约束任务游标
  *d_constraint_task_cursor_ = 0;

  VLOG(1) << "BuildConstraintTasks: prepared for " << num_cons
          << " constraints, " << num_worlds << " worlds";
}

void Batch3AManager::InitializeWorlds(int num_worlds) {
  const int num_vars = model_->GetNumVars();
  const int bit_dom_int_size = model_->GetBitDomIntSize();
  const int num_cons = model_->GetNumCons();
  const int dom_words_per_world = num_vars * bit_dom_int_size;
  const int bitmap_size_words = (num_cons + 31) / 32;

  // 初始化每个 world 的 workspace
  for (int w = 0; w < num_worlds; ++w) {
    WorldWorkspace& ws = d_workspaces_[w];

    // 恢复域快照到私有域
    cudaMemcpy(ws.bitDom, d_snapshot_,
               dom_words_per_world * sizeof(u32),
               cudaMemcpyDeviceToDevice);

    // 恢复域大小快照
    cudaMemcpy(ws.d_cur_dom_size, d_dom_size_snapshot_,
               num_vars * sizeof(int),
               cudaMemcpyDeviceToDevice);

    // 清空前沿位图
    cudaMemset(ws.frontier_A, 0, bitmap_size_words * sizeof(u32));
    cudaMemset(ws.frontier_B, 0, bitmap_size_words * sizeof(u32));

    // 重置控制状态
    ws.inconsistent_flag = 0;
    ws.scanner_index = 0;
    ws.deletions = 0;
    ws.iterations = 0;
    ws.frontier_nonempty = 0;
  }

  // 初始化全局控制
  *d_global_iteration_ = 0;
  *d_all_converged_flag_ = 0;
  *d_any_world_active_ = 1;  // 初始所有 world 都活跃
  // 所有 world 的位掩码（注意：num_worlds==32 时不能做 1U<<32）
  *d_active_world_mask_ =
      (num_worlds >= 32) ? 0xFFFFFFFFu : ((1u << num_worlds) - 1u);

  // 初始化结果数组（全部为 true，表示一致）
  for (int w = 0; w < num_worlds; ++w) {
    d_results_[w] = true;
  }

  // 清空统计
  *d_total_constraint_checks_ = 0;
  *d_total_deletions_ = 0;

  cudaDeviceSynchronize();

  VLOG(1) << "InitializeWorlds: initialized " << num_worlds << " worlds";
}

void Batch3AManager::LaunchBatch3AKernel(int num_worlds) {
  CHECK(num_worlds > 0 && num_worlds <= max_worlds_)
      << "num_worlds out of range: " << num_worlds;

  // 拷贝任务到 device buffer
  cudaMemcpy(d_tasks_, task_queue_.data(),
             num_worlds * sizeof(ProbeTask),
             cudaMemcpyHostToDevice);

  // 初始化全局控制（从 InitializeWorlds 下沉过来；避免 host 侧逐 world memcpy/memset）
  *d_global_iteration_ = 0;
  *d_all_converged_flag_ = 0;
  *d_any_world_active_ = 1;
  *d_active_world_mask_ =
      (num_worlds >= 32) ? 0xFFFFFFFFu : ((1u << num_worlds) - 1u);
  if (stats_enabled_) {
    *d_total_constraint_checks_ = 0;
    *d_total_deletions_ = 0;
  }

  // 清空 Dynamic Submission 队列与 mask（每次 kernel 启动前重置）
  const int num_cons = model_->GetNumCons();
  const size_t mask_bytes =
      static_cast<size_t>(kMaxBatch3ABlocks) * num_cons * sizeof(u32);
  cudaMemset(d_block_frontier_mask_A_, 0, mask_bytes);
  cudaMemset(d_block_frontier_mask_B_, 0, mask_bytes);
  cudaMemset(d_block_queue_tail_A_, 0, kMaxBatch3ABlocks * sizeof(int));
  cudaMemset(d_block_queue_tail_B_, 0, kMaxBatch3ABlocks * sizeof(int));
  cudaMemset(d_block_overflow_, 0, kMaxBatch3ABlocks * sizeof(int));

  // 初始化控制结构
  d_control_->num_constraint_tasks = model_->GetNumCons();
  d_control_->constraint_task_cursor = d_constraint_task_cursor_;
  d_control_->constraint_tasks = d_constraint_tasks_;

  d_control_->queue_capacity = queue_capacity_;
  d_control_->block_frontier_mask_A = d_block_frontier_mask_A_;
  d_control_->block_frontier_mask_B = d_block_frontier_mask_B_;
  d_control_->block_cid_queue_A = d_block_cid_queue_A_;
  d_control_->block_cid_queue_B = d_block_cid_queue_B_;
  d_control_->block_queue_tail_A = d_block_queue_tail_A_;
  d_control_->block_queue_tail_B = d_block_queue_tail_B_;
  d_control_->block_overflow = d_block_overflow_;

  d_control_->num_worlds = num_worlds;
  d_control_->world_probes = d_tasks_;
  d_control_->workspaces = d_workspaces_;
  d_control_->domain_snapshot = d_snapshot_;
  d_control_->dom_size_snapshot = d_dom_size_snapshot_;
  d_control_->results = d_results_;
  d_control_->global_iteration = d_global_iteration_;
  d_control_->all_converged_flag = d_all_converged_flag_;
  d_control_->any_world_active = d_any_world_active_;
  d_control_->active_world_mask = d_active_world_mask_;
  d_control_->num_blocks = num_blocks_;
  d_control_->max_iterations = max_iterations_;
  d_control_->activation_strategy = activation_strategy_;
  d_control_->check_mapping = static_cast<int>(check_mapping_);
  d_control_->subwarp_size = SanitizeSubwarpSize(subwarp_size_);
  d_control_->requested_worlds_per_block = requested_worlds_per_block_;
  d_control_->shmem_padding = shmem_padding_ ? 1 : 0;
  d_control_->total_constraint_checks = stats_enabled_ ? d_total_constraint_checks_ : nullptr;
  d_control_->total_deletions = stats_enabled_ ? d_total_deletions_ : nullptr;

  cudaDeviceSynchronize();

  // 获取 GModel 数据并启动 kernel
  GModelData model_data = model_->GetModelData();
  LaunchBatch3AKernelWrapper(model_data, d_control_);

  // 等待 kernel 完成
  cudaError_t err = cudaDeviceSynchronize();
  CHECK(err == cudaSuccess) << "Batch3A kernel failed: "
                            << cudaGetErrorString(err);
}

int Batch3AManager::CollectResults(int num_worlds,
                                   std::vector<int>& failed_vars,
                                   std::vector<int>& failed_values,
                                   std::vector<int>* unknown_vars,
                                   std::vector<int>* unknown_values) {
  int num_failed = 0;

  const u32 active_mask = d_active_world_mask_ ? *d_active_world_mask_ : 0u;

  for (int w = 0; w < num_worlds; ++w) {
    // 未在 max_iterations 内收敛：仍保留在 active_world_mask 中
    if (active_mask & (1u << w)) {
      if (unknown_vars != nullptr && unknown_values != nullptr) {
        unknown_vars->push_back(task_queue_[w].var_id);
        unknown_values->push_back(task_queue_[w].value);
      }
      continue;
    }
    if (!d_results_[w]) {
      failed_vars.push_back(task_queue_[w].var_id);
      failed_values.push_back(task_queue_[w].value);
      num_failed++;
    }
  }

  // 收集统计
  if (stats_enabled_) {
    total_constraint_checks_ = *d_total_constraint_checks_;
    total_deletions_ = *d_total_deletions_;
  }

  return num_failed;
}

int Batch3AManager::Execute(std::vector<int>& failed_vars,
                            std::vector<int>& failed_values,
                            std::vector<int>* unknown_vars,
                            std::vector<int>* unknown_values) {
  failed_vars.clear();
  failed_values.clear();
  if (unknown_vars != nullptr) unknown_vars->clear();
  if (unknown_values != nullptr) unknown_values->clear();

  const int num_tasks = static_cast<int>(task_queue_.size());
  if (num_tasks == 0) {
    return 0;
  }

  CHECK(memory_allocated_) << "Batch3AManager memory not allocated";

  // 检查是否适合使用 Batch-3A
  if (!IsSuitableForBatch3A()) {
    LOG(WARNING) << "Batch-3A: bitSup too large (" << GetBitSupSizePerConstraint()
                 << " bytes), should fallback to Stage 2";
    // 在 MVP 中，即使不适合也尝试运行，但发出警告
  }

  int total_failed = 0;

  // 按 max_worlds_ 分批处理
  for (int offset = 0; offset < num_tasks; offset += max_worlds_) {
    const int batch_size = std::min(max_worlds_, num_tasks - offset);

    VLOG(1) << "Batch3A: processing batch [" << offset << ", "
            << (offset + batch_size) << ") of " << num_tasks << " tasks";

    // 临时调整 task_queue_ 到当前批次
    std::vector<ProbeTask> saved_queue = task_queue_;
    task_queue_.clear();
    for (int i = offset; i < offset + batch_size; ++i) {
      task_queue_.push_back(saved_queue[i]);
    }

    // 保存快照（只在第一批时保存，后续批次快照相同）
    if (offset == 0) {
      SaveSnapshot();
    }

    // 构建约束任务
    BuildConstraintTasks(batch_size);

    // 启动 kernel
    LaunchBatch3AKernel(batch_size);

    // 收集结果
    std::vector<int> batch_failed_vars, batch_failed_values;
    std::vector<int> batch_unknown_vars, batch_unknown_values;
    std::vector<int>* batch_unknown_vars_ptr = nullptr;
    std::vector<int>* batch_unknown_values_ptr = nullptr;
    if (unknown_vars != nullptr && unknown_values != nullptr) {
      batch_unknown_vars_ptr = &batch_unknown_vars;
      batch_unknown_values_ptr = &batch_unknown_values;
    }
    int batch_failed = CollectResults(batch_size,
                                      batch_failed_vars,
                                      batch_failed_values,
                                      batch_unknown_vars_ptr,
                                      batch_unknown_values_ptr);

    // 合并结果
    for (size_t i = 0; i < batch_failed_vars.size(); ++i) {
      failed_vars.push_back(batch_failed_vars[i]);
      failed_values.push_back(batch_failed_values[i]);
    }
    total_failed += batch_failed;

    if (unknown_vars != nullptr && unknown_values != nullptr) {
      for (size_t i = 0; i < batch_unknown_vars.size(); ++i) {
        unknown_vars->push_back(batch_unknown_vars[i]);
        unknown_values->push_back(batch_unknown_values[i]);
      }
    }

    // 恢复 task_queue_
    task_queue_ = saved_queue;
  }

  VLOG(1) << "Batch3A completed: " << total_failed << " / " << num_tasks
          << " probes failed";

  if (stats_enabled_) {
    VLOG(1) << "Batch3A stats: constraint_checks=" << total_constraint_checks_
            << ", deletions=" << total_deletions_;
  }

  Clear();
  return total_failed;
}

bool Batch3AManager::IsSuitableForBatch3A() const {
  // MVP 限制：bitSup 必须能放入 shared memory
  // 典型 shared memory 大小：48KB (Jetson) 或 96KB (desktop GPU)
  // 留出一些空间给其他变量，限制 bitSup 为 38KB
  const int max_bitsup_bytes = 38 * 1024;
  return GetBitSupSizePerConstraint() <= max_bitsup_bytes;
}

int Batch3AManager::GetBitSupSizePerConstraint() const {
  const int max_dom_size = model_->max_dom_size;
  const int bit_dom_int_size = model_->GetBitDomIntSize();

  // bitSup 大小 = 2 * max_dom_size * bit_dom_int_size * sizeof(uint2)
  // 每个值对 (a, b) 需要一个 uint2 表示支持位图
  return 2 * max_dom_size * bit_dom_int_size * sizeof(uint2);
}

// ============================================================================
// FQ-PT Baseline 管理器实现
// ============================================================================

int FQPTBaselineManager::RoundUpPow2(int v) {
  if (v <= 1) return 1;
  int x = v - 1;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  return x + 1;
}

int FQPTBaselineManager::ComputeRecommendedNumBlocks(int device_id) {
  cudaDeviceProp prop;
  cudaError_t err = cudaGetDeviceProperties(&prop, device_id);
  if (err != cudaSuccess) {
    return 8;
  }
  return std::max(1, std::min(64, prop.multiProcessorCount * 2));
}

FQPTBaselineManager::FQPTBaselineManager(GModel* model, int num_blocks)
    : model_(model) {
  CHECK(model_ != nullptr) << "GModel pointer cannot be null";
  if (num_blocks == -1) {
    num_blocks_ = ComputeRecommendedNumBlocks(0);
  } else {
    CHECK(num_blocks > 0) << "num_blocks must be > 0 or -1";
    num_blocks_ = num_blocks;
  }

  max_tasks_ = std::max(64, num_blocks_ * 8);
  queue_capacity_ = RoundUpPow2(std::max(4096, model_->GetNumCons() * 8));
  AllocateMemory();
}

FQPTBaselineManager::~FQPTBaselineManager() {
  FreeMemory();
}

void FQPTBaselineManager::SetQueueCapacity(int capacity_pow2) {
  const int new_capacity = RoundUpPow2(std::max(32, capacity_pow2));
  if (new_capacity == queue_capacity_) return;
  queue_capacity_ = new_capacity;
  if (memory_allocated_) {
    FreeMemory();
    AllocateMemory();
  }
}

void FQPTBaselineManager::SetEnableWorldOwner(bool enabled) {
  if (enable_world_owner_ == enabled) return;
  enable_world_owner_ = enabled;
  if (memory_allocated_) {
    FreeMemory();
    AllocateMemory();
  }
}

void FQPTBaselineManager::SetEnableWorldStealing(bool enabled) {
  enable_world_stealing_ = enabled;
}

void FQPTBaselineManager::AllocateMemory() {
  if (memory_allocated_) return;

  const int num_vars = model_->GetNumVars();
  const int bit_dom_int_size = model_->GetBitDomIntSize();
  const int num_cons = model_->GetNumCons();
  const int dom_words_per_world = num_vars * bit_dom_int_size;
  const int bitmap_size_words = (num_cons + 31) / 32;

  cudaError_t err;

  err = cudaMallocManaged(&d_tasks_, max_tasks_ * sizeof(ProbeTask));
  CHECK(err == cudaSuccess) << "Failed to allocate d_tasks_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_world_results_, max_tasks_ * sizeof(bool));
  CHECK(err == cudaSuccess) << "Failed to allocate d_world_results_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_world_status_, max_tasks_ * sizeof(int));
  CHECK(err == cudaSuccess) << "Failed to allocate d_world_status_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(
      &d_snapshot_,
      static_cast<size_t>(dom_words_per_world) * sizeof(u32));
  CHECK(err == cudaSuccess) << "Failed to allocate d_snapshot_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_dom_size_snapshot_, num_vars * sizeof(int));
  CHECK(err == cudaSuccess) << "Failed to allocate d_dom_size_snapshot_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_workspaces_, max_tasks_ * sizeof(WorldWorkspace));
  CHECK(err == cudaSuccess) << "Failed to allocate d_workspaces_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(
      &d_ws_bitdom_,
      static_cast<size_t>(max_tasks_) * dom_words_per_world * sizeof(u32));
  CHECK(err == cudaSuccess) << "Failed to allocate d_ws_bitdom_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(
      &d_ws_dom_size_,
      static_cast<size_t>(max_tasks_) * num_vars * sizeof(int));
  CHECK(err == cudaSuccess) << "Failed to allocate d_ws_dom_size_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(
      &d_ws_frontier_A_,
      static_cast<size_t>(max_tasks_) * bitmap_size_words * sizeof(u32));
  CHECK(err == cudaSuccess) << "Failed to allocate d_ws_frontier_A_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(
      &d_ws_frontier_B_,
      static_cast<size_t>(max_tasks_) * bitmap_size_words * sizeof(u32));
  CHECK(err == cudaSuccess) << "Failed to allocate d_ws_frontier_B_: "
                            << cudaGetErrorString(err);

  if (!enable_world_owner_) {
    err = cudaMallocManaged(&d_world_locks_, max_tasks_ * sizeof(int));
    CHECK(err == cudaSuccess) << "Failed to allocate d_world_locks_: "
                              << cudaGetErrorString(err);

    err = cudaMallocManaged(&d_queue_slots_, queue_capacity_ * sizeof(FQPTRingSlot));
    CHECK(err == cudaSuccess) << "Failed to allocate d_queue_slots_: "
                              << cudaGetErrorString(err);

    err = cudaMallocManaged(&d_enqueue_pos_, sizeof(unsigned long long));
    CHECK(err == cudaSuccess) << "Failed to allocate d_enqueue_pos_: "
                              << cudaGetErrorString(err);
    err = cudaMallocManaged(&d_dequeue_pos_, sizeof(unsigned long long));
    CHECK(err == cudaSuccess) << "Failed to allocate d_dequeue_pos_: "
                              << cudaGetErrorString(err);
    err = cudaMallocManaged(&d_pending_tasks_, sizeof(unsigned long long));
    CHECK(err == cudaSuccess) << "Failed to allocate d_pending_tasks_: "
                              << cudaGetErrorString(err);
  }
  err = cudaMallocManaged(&d_processed_tasks_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_processed_tasks_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_overflow_count_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_overflow_count_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_unknown_count_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_unknown_count_: "
                            << cudaGetErrorString(err);

  err = cudaMallocManaged(&d_total_constraint_checks_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_total_constraint_checks_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_total_deletions_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_total_deletions_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_stale_drop_count_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_stale_drop_count_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_lock_fail_count_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_lock_fail_count_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_lock_retry_count_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_lock_retry_count_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_bucket_count_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_bucket_count_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_bucket_task_sum_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_bucket_task_sum_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_bucket_active_warp_sum_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_bucket_active_warp_sum_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_frontier_pop_count_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_frontier_pop_count_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_frontier_scan_steps_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_frontier_scan_steps_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_ow1_scatter_calls_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_ow1_scatter_calls_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_ow1_fallback_calls_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_ow1_fallback_calls_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_ow1_word_leader_writes_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_ow1_word_leader_writes_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_microbatch_rounds_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_microbatch_rounds_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_microbatch_sel_ge2_rounds_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_microbatch_sel_ge2_rounds_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_microbatch_sel_sum_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_microbatch_sel_sum_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_microbatch_aligned_rounds_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_microbatch_aligned_rounds_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_microbatch_degrade_rounds_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_microbatch_degrade_rounds_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_microbatch_parked_warps_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_microbatch_parked_warps_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_microbatch_round_cap_fallbacks_, sizeof(unsigned long long));
  CHECK(err == cudaSuccess) << "Failed to allocate d_microbatch_round_cap_fallbacks_: "
                            << cudaGetErrorString(err);
  err = cudaMallocManaged(&d_world_cursor_, sizeof(unsigned int));
  CHECK(err == cudaSuccess) << "Failed to allocate d_world_cursor_: "
                            << cudaGetErrorString(err);
  *d_world_cursor_ = 0u;

  err = cudaMallocManaged(&d_control_, sizeof(FQPTControl));
  CHECK(err == cudaSuccess) << "Failed to allocate d_control_: "
                            << cudaGetErrorString(err);

  for (int i = 0; i < max_tasks_; ++i) {
    WorldWorkspace& ws = d_workspaces_[i];
    ws.bitDom = d_ws_bitdom_ + static_cast<size_t>(i) * dom_words_per_world;
    ws.d_cur_dom_size = d_ws_dom_size_ + static_cast<size_t>(i) * num_vars;
    ws.frontier_A =
        d_ws_frontier_A_ + static_cast<size_t>(i) * bitmap_size_words;
    ws.frontier_B =
        d_ws_frontier_B_ + static_cast<size_t>(i) * bitmap_size_words;
    ws.inconsistent_flag = 0;
    ws.scanner_index = 0;
    ws.deletions = 0;
    ws.iterations = 0;
    ws.frontier_nonempty = 0;
  }

  memory_allocated_ = true;
}

void FQPTBaselineManager::FreeMemory() {
  if (!memory_allocated_) return;

  if (d_tasks_) cudaFree(d_tasks_);
  if (d_world_results_) cudaFree(d_world_results_);
  if (d_world_status_) cudaFree(d_world_status_);
  if (d_snapshot_) cudaFree(d_snapshot_);
  if (d_dom_size_snapshot_) cudaFree(d_dom_size_snapshot_);
  if (d_workspaces_) cudaFree(d_workspaces_);
  if (d_ws_bitdom_) cudaFree(d_ws_bitdom_);
  if (d_ws_dom_size_) cudaFree(d_ws_dom_size_);
  if (d_ws_frontier_A_) cudaFree(d_ws_frontier_A_);
  if (d_ws_frontier_B_) cudaFree(d_ws_frontier_B_);
  if (d_world_locks_) cudaFree(d_world_locks_);
  if (d_queue_slots_) cudaFree(d_queue_slots_);
  if (d_enqueue_pos_) cudaFree(d_enqueue_pos_);
  if (d_dequeue_pos_) cudaFree(d_dequeue_pos_);
  if (d_pending_tasks_) cudaFree(d_pending_tasks_);
  if (d_processed_tasks_) cudaFree(d_processed_tasks_);
  if (d_overflow_count_) cudaFree(d_overflow_count_);
  if (d_unknown_count_) cudaFree(d_unknown_count_);
  if (d_total_constraint_checks_) cudaFree(d_total_constraint_checks_);
  if (d_total_deletions_) cudaFree(d_total_deletions_);
  if (d_stale_drop_count_) cudaFree(d_stale_drop_count_);
  if (d_lock_fail_count_) cudaFree(d_lock_fail_count_);
  if (d_lock_retry_count_) cudaFree(d_lock_retry_count_);
  if (d_bucket_count_) cudaFree(d_bucket_count_);
  if (d_bucket_task_sum_) cudaFree(d_bucket_task_sum_);
  if (d_bucket_active_warp_sum_) cudaFree(d_bucket_active_warp_sum_);
  if (d_frontier_pop_count_) cudaFree(d_frontier_pop_count_);
  if (d_frontier_scan_steps_) cudaFree(d_frontier_scan_steps_);
  if (d_ow1_scatter_calls_) cudaFree(d_ow1_scatter_calls_);
  if (d_ow1_fallback_calls_) cudaFree(d_ow1_fallback_calls_);
  if (d_ow1_word_leader_writes_) cudaFree(d_ow1_word_leader_writes_);
  if (d_microbatch_rounds_) cudaFree(d_microbatch_rounds_);
  if (d_microbatch_sel_ge2_rounds_) cudaFree(d_microbatch_sel_ge2_rounds_);
  if (d_microbatch_sel_sum_) cudaFree(d_microbatch_sel_sum_);
  if (d_microbatch_aligned_rounds_) cudaFree(d_microbatch_aligned_rounds_);
  if (d_microbatch_degrade_rounds_) cudaFree(d_microbatch_degrade_rounds_);
  if (d_microbatch_parked_warps_) cudaFree(d_microbatch_parked_warps_);
  if (d_microbatch_round_cap_fallbacks_) cudaFree(d_microbatch_round_cap_fallbacks_);
  if (d_world_cursor_) cudaFree(d_world_cursor_);
  if (d_control_) cudaFree(d_control_);

  d_tasks_ = nullptr;
  d_world_results_ = nullptr;
  d_world_status_ = nullptr;
  d_snapshot_ = nullptr;
  d_dom_size_snapshot_ = nullptr;
  d_workspaces_ = nullptr;
  d_ws_bitdom_ = nullptr;
  d_ws_dom_size_ = nullptr;
  d_ws_frontier_A_ = nullptr;
  d_ws_frontier_B_ = nullptr;
  d_world_locks_ = nullptr;
  d_queue_slots_ = nullptr;
  d_enqueue_pos_ = nullptr;
  d_dequeue_pos_ = nullptr;
  d_pending_tasks_ = nullptr;
  d_processed_tasks_ = nullptr;
  d_overflow_count_ = nullptr;
  d_unknown_count_ = nullptr;
  d_total_constraint_checks_ = nullptr;
  d_total_deletions_ = nullptr;
  d_stale_drop_count_ = nullptr;
  d_lock_fail_count_ = nullptr;
  d_lock_retry_count_ = nullptr;
  d_bucket_count_ = nullptr;
  d_bucket_task_sum_ = nullptr;
  d_bucket_active_warp_sum_ = nullptr;
  d_frontier_pop_count_ = nullptr;
  d_frontier_scan_steps_ = nullptr;
  d_ow1_scatter_calls_ = nullptr;
  d_ow1_fallback_calls_ = nullptr;
  d_ow1_word_leader_writes_ = nullptr;
  d_microbatch_rounds_ = nullptr;
  d_microbatch_sel_ge2_rounds_ = nullptr;
  d_microbatch_sel_sum_ = nullptr;
  d_microbatch_aligned_rounds_ = nullptr;
  d_microbatch_degrade_rounds_ = nullptr;
  d_microbatch_parked_warps_ = nullptr;
  d_microbatch_round_cap_fallbacks_ = nullptr;
  d_world_cursor_ = nullptr;
  d_control_ = nullptr;
  memory_allocated_ = false;
}

void FQPTBaselineManager::EnsureTaskCapacity(int required_tasks) {
  if (required_tasks <= max_tasks_) return;
  max_tasks_ = std::max(required_tasks, max_tasks_ * 2);
  FreeMemory();
  AllocateMemory();
}

void FQPTBaselineManager::AddTask(int var_id, int value) {
  const int num_vars = model_->GetNumVars();
  if (var_id < 0 || var_id >= num_vars) return;
  task_queue_.emplace_back(var_id, value, static_cast<int>(task_queue_.size()));
}

void FQPTBaselineManager::Clear() {
  task_queue_.clear();
}

void FQPTBaselineManager::SaveSnapshot() {
  const int num_vars = model_->GetNumVars();
  const int bit_dom_int_size = model_->GetBitDomIntSize();
  const int snapshot_size_words = num_vars * bit_dom_int_size;

  cudaError_t err = cudaMemcpy(
      d_snapshot_,
      model_->GetBitDom(),
      snapshot_size_words * sizeof(u32),
      cudaMemcpyDeviceToDevice);
  CHECK(err == cudaSuccess) << "Failed to save domain snapshot: "
                            << cudaGetErrorString(err);

  err = cudaMemcpy(
      d_dom_size_snapshot_,
      model_->GetDomainSizesPtr(),
      num_vars * sizeof(int),
      cudaMemcpyDeviceToDevice);
  CHECK(err == cudaSuccess) << "Failed to save dom_size snapshot: "
                            << cudaGetErrorString(err);

  err = cudaDeviceSynchronize();
  CHECK(err == cudaSuccess) << "cudaDeviceSynchronize failed after snapshot: "
                            << cudaGetErrorString(err);
}

void FQPTBaselineManager::InitializeWorldsFromSnapshot(int num_worlds) {
  const int num_vars = model_->GetNumVars();
  const int bit_dom_int_size = model_->GetBitDomIntSize();
  const int num_cons = model_->GetNumCons();
  const int dom_words_per_world = num_vars * bit_dom_int_size;
  const int bitmap_size_words = (num_cons + 31) / 32;

  cudaError_t err = cudaMemcpy(
      d_tasks_,
      task_queue_.data(),
      num_worlds * sizeof(ProbeTask),
      cudaMemcpyHostToDevice);
  CHECK(err == cudaSuccess) << "Failed to copy probe tasks: "
                            << cudaGetErrorString(err);

  for (int w = 0; w < num_worlds; ++w) {
    WorldWorkspace& ws = d_workspaces_[w];
    err = cudaMemcpy(
        ws.bitDom,
        d_snapshot_,
        dom_words_per_world * sizeof(u32),
        cudaMemcpyDeviceToDevice);
    CHECK(err == cudaSuccess) << "Failed to init world bitDom: "
                              << cudaGetErrorString(err);

    err = cudaMemcpy(
        ws.d_cur_dom_size,
        d_dom_size_snapshot_,
        num_vars * sizeof(int),
        cudaMemcpyDeviceToDevice);
    CHECK(err == cudaSuccess) << "Failed to init world dom_size: "
                              << cudaGetErrorString(err);
  }

  err = cudaDeviceSynchronize();
  CHECK(err == cudaSuccess) << "cudaDeviceSynchronize failed after world init: "
                            << cudaGetErrorString(err);

  for (int w = 0; w < num_worlds; ++w) {
    d_world_locks_[w] = 0;
    d_world_results_[w] = true;
    d_world_status_[w] = static_cast<int>(ProbeStatus::kOK);

    WorldWorkspace& ws = d_workspaces_[w];
    ws.inconsistent_flag = 0;
    ws.scanner_index = 0;
    ws.deletions = 0;
    ws.iterations = 0;
    ws.frontier_nonempty = 0;
    for (int bw = 0; bw < bitmap_size_words; ++bw) {
      ws.frontier_A[bw] = 0u;
      ws.frontier_B[bw] = 0u;
    }

    const ProbeTask& task = task_queue_[w];
    const int var = task.var_id;
    const int val = task.value;
    if (var < 0 || var >= num_vars || val < 0 || val >= model_->max_dom_size) {
      d_world_results_[w] = false;
      d_world_status_[w] = static_cast<int>(ProbeStatus::kDWO);
      ws.inconsistent_flag = 1;
      continue;
    }

    const int word = val / 32;
    const int bit = val % 32;
    if (word < 0 || word >= bit_dom_int_size) {
      d_world_results_[w] = false;
      d_world_status_[w] = static_cast<int>(ProbeStatus::kDWO);
      ws.inconsistent_flag = 1;
      continue;
    }

    const int base = var * bit_dom_int_size;
    const u32 old_word = ws.bitDom[base + word];
    if ((old_word & (1u << bit)) == 0u) {
      d_world_results_[w] = false;
      d_world_status_[w] = static_cast<int>(ProbeStatus::kDWO);
      ws.inconsistent_flag = 1;
      continue;
    }

    for (int k = 0; k < bit_dom_int_size; ++k) {
      ws.bitDom[base + k] = 0u;
    }
    ws.bitDom[base + word] = (1u << bit);
    ws.d_cur_dom_size[var] = 1;
  }

  for (int w = num_worlds; w < max_tasks_; ++w) {
    d_world_locks_[w] = 0;
    d_world_results_[w] = true;
    d_world_status_[w] = static_cast<int>(ProbeStatus::kOK);
    WorldWorkspace& ws = d_workspaces_[w];
    for (int bw = 0; bw < bitmap_size_words; ++bw) {
      ws.frontier_A[bw] = 0u;
      ws.frontier_B[bw] = 0u;
    }
  }

  *d_processed_tasks_ = 0;
  *d_total_constraint_checks_ = 0;
  *d_total_deletions_ = 0;
  *d_overflow_count_ = 0;
  *d_stale_drop_count_ = 0;
  *d_lock_fail_count_ = 0;
  *d_lock_retry_count_ = 0;
  *d_bucket_count_ = 0;
  *d_bucket_task_sum_ = 0;
  *d_bucket_active_warp_sum_ = 0;
  *d_frontier_pop_count_ = 0;
  *d_frontier_scan_steps_ = 0;
  *d_ow1_scatter_calls_ = 0;
  *d_ow1_fallback_calls_ = 0;
  *d_ow1_word_leader_writes_ = 0;
  *d_microbatch_rounds_ = 0;
  *d_microbatch_sel_ge2_rounds_ = 0;
  *d_microbatch_sel_sum_ = 0;
  *d_microbatch_aligned_rounds_ = 0;
  *d_microbatch_degrade_rounds_ = 0;
  *d_microbatch_parked_warps_ = 0;
  *d_microbatch_round_cap_fallbacks_ = 0;
}

void FQPTBaselineManager::InitializeGlobalQueueWithSeedTasks(int num_worlds) {
  std::vector<FQPTTask> seeds;
  seeds.reserve(std::min(queue_capacity_, num_worlds * 16));

  unsigned long long init_unknown = 0;
  unsigned long long init_overflow = 0;

  GModelData md = model_->GetModelData();
  for (int w = 0; w < num_worlds; ++w) {
    if (d_world_status_[w] != static_cast<int>(ProbeStatus::kOK)) continue;
    WorldWorkspace& ws = d_workspaces_[w];

    const int probe_var = task_queue_[w].var_id;
    const int start = md.d_subscription_offset[probe_var];
    const int end = md.d_subscription_offset[probe_var + 1];

    bool world_overflow = false;
    for (int i = start; i < end; ++i) {
      const int cid = md.d_subscription[i].z;
      const int word = cid >> 5;
      const u32 bit = (1u << (cid & 31));
      if ((ws.frontier_A[word] & bit) != 0u) {
        continue;
      }
      if (static_cast<int>(seeds.size()) >= queue_capacity_) {
        world_overflow = true;
        break;
      }
      ws.frontier_A[word] |= bit;
      seeds.emplace_back(w, cid);
    }
    if (world_overflow) {
      d_world_status_[w] = static_cast<int>(ProbeStatus::kUNKNOWN);
      d_world_results_[w] = true;
      ++init_unknown;
      ++init_overflow;
    }
  }

  for (int i = 0; i < queue_capacity_; ++i) {
    d_queue_slots_[i].seq = static_cast<unsigned long long>(i);
    d_queue_slots_[i].task = FQPTTask();
  }

  for (size_t i = 0; i < seeds.size(); ++i) {
    d_queue_slots_[i].task = seeds[i];
    d_queue_slots_[i].seq = static_cast<unsigned long long>(i + 1);
  }

  *d_enqueue_pos_ = static_cast<unsigned long long>(seeds.size());
  *d_dequeue_pos_ = 0ULL;
  *d_pending_tasks_ = static_cast<unsigned long long>(seeds.size());
  *d_processed_tasks_ = 0ULL;
  *d_overflow_count_ = init_overflow;
  *d_unknown_count_ = init_unknown;
}

void FQPTBaselineManager::LaunchKernel(int num_worlds) {
  d_control_->num_worlds = num_worlds;
  d_control_->world_probes = d_tasks_;
  d_control_->workspaces = d_workspaces_;
  d_control_->domain_snapshot = d_snapshot_;
  d_control_->dom_size_snapshot = d_dom_size_snapshot_;
  d_control_->world_results = d_world_results_;
  d_control_->world_status = d_world_status_;
  d_control_->world_locks = enable_world_owner_ ? nullptr : d_world_locks_;

  d_control_->queue_slots = enable_world_owner_ ? nullptr : d_queue_slots_;
  d_control_->enqueue_pos = enable_world_owner_ ? nullptr : d_enqueue_pos_;
  d_control_->dequeue_pos = enable_world_owner_ ? nullptr : d_dequeue_pos_;
  d_control_->queue_capacity = enable_world_owner_ ? 0 : queue_capacity_;
  d_control_->queue_mask = enable_world_owner_ ? 0 : (queue_capacity_ - 1);

  d_control_->pending_tasks = enable_world_owner_ ? nullptr : d_pending_tasks_;
  d_control_->processed_tasks = d_processed_tasks_;
  d_control_->overflow_count = enable_world_owner_ ? nullptr : d_overflow_count_;
  d_control_->unknown_count = d_unknown_count_;

  d_control_->cta_pop_batch = cta_pop_batch_;
  d_control_->local_buffer_capacity = local_buffer_capacity_;
  d_control_->lock_retry_limit = lock_retry_limit_;
  d_control_->lock_backoff = lock_backoff_;
  d_control_->enable_cid_grouping = enable_cid_grouping_ ? 1 : 0;
  d_control_->enable_parallel_group_check = enable_parallel_group_check_ ? 1 : 0;
  d_control_->group_warps_per_cta = group_warps_per_cta_;
  d_control_->group_degrade_threshold = group_degrade_threshold_;
  d_control_->enable_world_owner = enable_world_owner_ ? 1 : 0;
  const bool effective_microbatch = enable_world_owner_ && enable_cid_microbatch_;
  const bool effective_world_stealing =
      enable_world_owner_ && enable_world_stealing_ && !effective_microbatch;
  if (enable_world_owner_ && enable_world_stealing_ && effective_microbatch) {
    LOG_FIRST_N(WARNING, 1)
        << "FQ-PT OW3b: cid micro-batch enabled, world_stealing is disabled.";
  }
  d_control_->enable_world_stealing =
      effective_world_stealing ? 1 : 0;
  d_control_->world_cursor =
      effective_world_stealing ? d_world_cursor_ : nullptr;
  d_control_->enable_ow1_frontier_scatter =
      (enable_world_owner_ && enable_ow1_frontier_scatter_) ? 1 : 0;
  d_control_->ow1_min_degree = ow1_min_degree_;
  d_control_->ow1_scatter_mode = ow1_scatter_mode_;
  d_control_->ow1_force_scatter = ow1_force_scatter_ ? 1 : 0;
  d_control_->enable_cid_microbatch = effective_microbatch ? 1 : 0;
  d_control_->microbatch_min_sel = microbatch_min_sel_;
  d_control_->microbatch_warps = microbatch_warps_;
  d_control_->microbatch_max_rounds = microbatch_max_rounds_;
  d_control_->enable_cid_microbatch_profile =
      (enable_world_owner_ && enable_cid_microbatch_profile_) ? 1 : 0;
  d_control_->microbatch_profile_interval = microbatch_profile_interval_;

  d_control_->total_constraint_checks =
      stats_enabled_ ? d_total_constraint_checks_ : nullptr;
  d_control_->total_deletions =
      stats_enabled_ ? d_total_deletions_ : nullptr;
  d_control_->stale_drop_count =
      (stats_enabled_ && !enable_world_owner_) ? d_stale_drop_count_ : nullptr;
  d_control_->lock_fail_count =
      (stats_enabled_ && !enable_world_owner_) ? d_lock_fail_count_ : nullptr;
  d_control_->lock_retry_count =
      (stats_enabled_ && !enable_world_owner_) ? d_lock_retry_count_ : nullptr;
  d_control_->bucket_count =
      (stats_enabled_ && !enable_world_owner_) ? d_bucket_count_ : nullptr;
  d_control_->bucket_task_sum =
      (stats_enabled_ && !enable_world_owner_) ? d_bucket_task_sum_ : nullptr;
  d_control_->bucket_active_warp_sum =
      (stats_enabled_ && !enable_world_owner_) ? d_bucket_active_warp_sum_ : nullptr;
  d_control_->frontier_pop_count =
      stats_enabled_ ? d_frontier_pop_count_ : nullptr;
  d_control_->frontier_scan_steps =
      stats_enabled_ ? d_frontier_scan_steps_ : nullptr;
  d_control_->ow1_scatter_calls =
      (stats_enabled_ && enable_world_owner_) ? d_ow1_scatter_calls_ : nullptr;
  d_control_->ow1_fallback_calls =
      (stats_enabled_ && enable_world_owner_) ? d_ow1_fallback_calls_ : nullptr;
  d_control_->ow1_word_leader_writes =
      (stats_enabled_ && enable_world_owner_) ? d_ow1_word_leader_writes_ : nullptr;
  d_control_->microbatch_rounds =
      (stats_enabled_ && enable_world_owner_ && enable_cid_microbatch_profile_)
          ? d_microbatch_rounds_
          : nullptr;
  d_control_->microbatch_sel_ge2_rounds =
      (stats_enabled_ && enable_world_owner_ && enable_cid_microbatch_profile_)
          ? d_microbatch_sel_ge2_rounds_
          : nullptr;
  d_control_->microbatch_sel_sum =
      (stats_enabled_ && enable_world_owner_ && enable_cid_microbatch_profile_)
          ? d_microbatch_sel_sum_
          : nullptr;
  d_control_->microbatch_aligned_rounds =
      (stats_enabled_ && enable_world_owner_ && effective_microbatch)
          ? d_microbatch_aligned_rounds_
          : nullptr;
  d_control_->microbatch_degrade_rounds =
      (stats_enabled_ && enable_world_owner_ && effective_microbatch)
          ? d_microbatch_degrade_rounds_
          : nullptr;
  d_control_->microbatch_parked_warps =
      (stats_enabled_ && enable_world_owner_ && effective_microbatch)
          ? d_microbatch_parked_warps_
          : nullptr;
  d_control_->microbatch_round_cap_fallbacks =
      (stats_enabled_ && enable_world_owner_ && effective_microbatch)
          ? d_microbatch_round_cap_fallbacks_
          : nullptr;

  cudaError_t err = cudaDeviceSynchronize();
  CHECK(err == cudaSuccess) << "cudaDeviceSynchronize failed before FQPT kernel: "
                            << cudaGetErrorString(err);

  if (enable_world_owner_) {
    LaunchFQPTOwnerFrontierKernelWrapper(model_->GetModelData(), d_control_, num_blocks_);
  } else {
    LaunchFQPTBaselineKernelWrapper(model_->GetModelData(), d_control_, num_blocks_);
  }

  err = cudaDeviceSynchronize();
  CHECK(err == cudaSuccess) << "FQPT kernel failed: " << cudaGetErrorString(err);
}

int FQPTBaselineManager::CollectResults(
    int num_worlds,
    std::vector<int>& failed_vars,
    std::vector<int>& failed_values,
    std::vector<int>* unknown_vars,
    std::vector<int>* unknown_values) {
  failed_vars.clear();
  failed_values.clear();
  if (unknown_vars != nullptr) unknown_vars->clear();
  if (unknown_values != nullptr) unknown_values->clear();

  last_stats_ = FQPTStatistics{};
  last_stats_.total_worlds = num_worlds;
  last_stats_.processed_tasks = d_processed_tasks_ ? *d_processed_tasks_ : 0ULL;
  last_stats_.overflow_count = d_overflow_count_ ? *d_overflow_count_ : 0ULL;
  last_stats_.constraint_checks =
      stats_enabled_ && d_total_constraint_checks_ ? *d_total_constraint_checks_ : 0ULL;
  last_stats_.deletions =
      stats_enabled_ && d_total_deletions_ ? *d_total_deletions_ : 0ULL;
  last_stats_.stale_drop_count =
      stats_enabled_ && d_stale_drop_count_ ? *d_stale_drop_count_ : 0ULL;
  last_stats_.lock_fail_count =
      stats_enabled_ && d_lock_fail_count_ ? *d_lock_fail_count_ : 0ULL;
  last_stats_.lock_retry_count =
      stats_enabled_ && d_lock_retry_count_ ? *d_lock_retry_count_ : 0ULL;
  const unsigned long long bucket_count =
      stats_enabled_ && d_bucket_count_ ? *d_bucket_count_ : 0ULL;
  const unsigned long long bucket_task_sum =
      stats_enabled_ && d_bucket_task_sum_ ? *d_bucket_task_sum_ : 0ULL;
  const unsigned long long bucket_active_warp_sum =
      stats_enabled_ && d_bucket_active_warp_sum_ ? *d_bucket_active_warp_sum_ : 0ULL;
  if (bucket_count > 0ULL) {
    last_stats_.avg_bucket_size =
        static_cast<double>(bucket_task_sum) / static_cast<double>(bucket_count);
    const int denom_warps = std::max(1, group_warps_per_cta_);
    last_stats_.avg_bucket_utilization =
        static_cast<double>(bucket_active_warp_sum) /
        static_cast<double>(bucket_count * static_cast<unsigned long long>(denom_warps));
  }
  last_stats_.frontier_pop_count =
      stats_enabled_ && d_frontier_pop_count_ ? *d_frontier_pop_count_ : 0ULL;
  last_stats_.frontier_scan_steps =
      stats_enabled_ && d_frontier_scan_steps_ ? *d_frontier_scan_steps_ : 0ULL;
  if (last_stats_.frontier_pop_count > 0ULL) {
    last_stats_.avg_frontier_scan_steps =
        static_cast<double>(last_stats_.frontier_scan_steps) /
        static_cast<double>(last_stats_.frontier_pop_count);
  }
  last_stats_.ow1_scatter_calls =
      stats_enabled_ && d_ow1_scatter_calls_ ? *d_ow1_scatter_calls_ : 0ULL;
  last_stats_.ow1_fallback_calls =
      stats_enabled_ && d_ow1_fallback_calls_ ? *d_ow1_fallback_calls_ : 0ULL;
  last_stats_.ow1_word_leader_writes =
      stats_enabled_ && d_ow1_word_leader_writes_ ? *d_ow1_word_leader_writes_ : 0ULL;
  last_stats_.microbatch_rounds =
      stats_enabled_ && d_microbatch_rounds_ ? *d_microbatch_rounds_ : 0ULL;
  last_stats_.microbatch_sel_ge2_rounds =
      stats_enabled_ && d_microbatch_sel_ge2_rounds_
          ? *d_microbatch_sel_ge2_rounds_
          : 0ULL;
  last_stats_.microbatch_sel_sum =
      stats_enabled_ && d_microbatch_sel_sum_ ? *d_microbatch_sel_sum_ : 0ULL;
  if (last_stats_.microbatch_rounds > 0ULL) {
    last_stats_.avg_sel_count =
        static_cast<double>(last_stats_.microbatch_sel_sum) /
        static_cast<double>(last_stats_.microbatch_rounds);
  }
  last_stats_.microbatch_aligned_rounds =
      stats_enabled_ && d_microbatch_aligned_rounds_
          ? *d_microbatch_aligned_rounds_
          : 0ULL;
  last_stats_.microbatch_degrade_rounds =
      stats_enabled_ && d_microbatch_degrade_rounds_
          ? *d_microbatch_degrade_rounds_
          : 0ULL;
  last_stats_.microbatch_parked_warps =
      stats_enabled_ && d_microbatch_parked_warps_
          ? *d_microbatch_parked_warps_
          : 0ULL;
  last_stats_.microbatch_round_cap_fallbacks =
      stats_enabled_ && d_microbatch_round_cap_fallbacks_
          ? *d_microbatch_round_cap_fallbacks_
          : 0ULL;
  const unsigned long long total_mb_exec_rounds =
      last_stats_.microbatch_aligned_rounds + last_stats_.microbatch_degrade_rounds;
  if (total_mb_exec_rounds > 0ULL) {
    last_stats_.microbatch_align_ratio =
        static_cast<double>(last_stats_.microbatch_aligned_rounds) /
        static_cast<double>(total_mb_exec_rounds);
    last_stats_.microbatch_parked_per_round =
        static_cast<double>(last_stats_.microbatch_parked_warps) /
        static_cast<double>(total_mb_exec_rounds);
  }

  int failed = 0;
  for (int w = 0; w < num_worlds; ++w) {
    const int status = d_world_status_[w];
    if (status == static_cast<int>(ProbeStatus::kDWO)) {
      failed_vars.push_back(task_queue_[w].var_id);
      failed_values.push_back(task_queue_[w].value);
      ++failed;
      ++last_stats_.dwo_worlds;
    } else if (status == static_cast<int>(ProbeStatus::kUNKNOWN)) {
      if (unknown_vars != nullptr && unknown_values != nullptr) {
        unknown_vars->push_back(task_queue_[w].var_id);
        unknown_values->push_back(task_queue_[w].value);
      }
      ++last_stats_.unknown_worlds;
    } else {
      ++last_stats_.ok_worlds;
    }
  }
  return failed;
}

int FQPTBaselineManager::Execute(
    std::vector<int>& failed_vars,
    std::vector<int>& failed_values,
    std::vector<int>* unknown_vars,
    std::vector<int>* unknown_values) {
  const int num_tasks = static_cast<int>(task_queue_.size());
  if (num_tasks == 0) {
    failed_vars.clear();
    failed_values.clear();
    if (unknown_vars != nullptr) unknown_vars->clear();
    if (unknown_values != nullptr) unknown_values->clear();
    last_stats_ = FQPTStatistics{};
    return 0;
  }

  EnsureTaskCapacity(num_tasks);
  SaveSnapshot();
  if (d_processed_tasks_ != nullptr) *d_processed_tasks_ = 0ULL;
  if (d_total_constraint_checks_ != nullptr) *d_total_constraint_checks_ = 0ULL;
  if (d_total_deletions_ != nullptr) *d_total_deletions_ = 0ULL;
  if (d_overflow_count_ != nullptr) *d_overflow_count_ = 0ULL;
  if (d_unknown_count_ != nullptr) *d_unknown_count_ = 0ULL;
  if (d_stale_drop_count_ != nullptr) *d_stale_drop_count_ = 0ULL;
  if (d_lock_fail_count_ != nullptr) *d_lock_fail_count_ = 0ULL;
  if (d_lock_retry_count_ != nullptr) *d_lock_retry_count_ = 0ULL;
  if (d_bucket_count_ != nullptr) *d_bucket_count_ = 0ULL;
  if (d_bucket_task_sum_ != nullptr) *d_bucket_task_sum_ = 0ULL;
  if (d_bucket_active_warp_sum_ != nullptr) *d_bucket_active_warp_sum_ = 0ULL;
  if (d_frontier_pop_count_ != nullptr) *d_frontier_pop_count_ = 0ULL;
  if (d_frontier_scan_steps_ != nullptr) *d_frontier_scan_steps_ = 0ULL;
  if (d_ow1_scatter_calls_ != nullptr) *d_ow1_scatter_calls_ = 0ULL;
  if (d_ow1_fallback_calls_ != nullptr) *d_ow1_fallback_calls_ = 0ULL;
  if (d_ow1_word_leader_writes_ != nullptr) *d_ow1_word_leader_writes_ = 0ULL;
  if (d_microbatch_rounds_ != nullptr) *d_microbatch_rounds_ = 0ULL;
  if (d_microbatch_sel_ge2_rounds_ != nullptr) *d_microbatch_sel_ge2_rounds_ = 0ULL;
  if (d_microbatch_sel_sum_ != nullptr) *d_microbatch_sel_sum_ = 0ULL;
  if (d_microbatch_aligned_rounds_ != nullptr) *d_microbatch_aligned_rounds_ = 0ULL;
  if (d_microbatch_degrade_rounds_ != nullptr) *d_microbatch_degrade_rounds_ = 0ULL;
  if (d_microbatch_parked_warps_ != nullptr) *d_microbatch_parked_warps_ = 0ULL;
  if (d_microbatch_round_cap_fallbacks_ != nullptr) {
    *d_microbatch_round_cap_fallbacks_ = 0ULL;
  }
  if (d_world_cursor_ != nullptr) *d_world_cursor_ = 0u;

  if (!enable_world_owner_) {
    InitializeWorldsFromSnapshot(num_tasks);
    InitializeGlobalQueueWithSeedTasks(num_tasks);
  } else {
    cudaError_t err = cudaMemcpy(
        d_tasks_,
        task_queue_.data(),
        num_tasks * sizeof(ProbeTask),
        cudaMemcpyHostToDevice);
    CHECK(err == cudaSuccess) << "Failed to copy probe tasks: "
                              << cudaGetErrorString(err);
  }
  LaunchKernel(num_tasks);
  const int failed = CollectResults(
      num_tasks, failed_vars, failed_values, unknown_vars, unknown_values);
  Clear();
  return failed;
}

}  // namespace cpim
