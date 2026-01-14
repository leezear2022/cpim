# Batch AC-GPU 未完成工作清单

> 基于 [BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md](BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md) 的实现进度追踪
>
> **最后更新**: 2026-01-02
>
> **当前状态**: Phase 2（P0 完成，P1/P2/P3 待做），Phase 3 全部完成（Stage 1 + Stage 2），Stage 2 正确性已验证但性能待优化

---

## 📊 完成度总览

| 阶段 | 状态 | 完成度 | 优先级 | 预期收益 |
|------|------|--------|--------|----------|
| Phase 1: Batch-1 基础 | ✅ 完成 | 100% | - | Baseline |
| Phase 2: Batch-1 优化 | 🟡 部分完成 | 50% | ⭐⭐⭐ 高 | 2-3× 吞吐（以实测为准） |
| Phase 3: Batch-2 基础 | ✅ 完成 | 100% | ⭐⭐⭐ 高 | **Stage 1: 4.5-11× 吞吐**；Stage 2: 正确但待优化 |
| Phase 4: Batch-2 优化 | ❌ 未开始 | 0% | ⭐⭐ 中 | 内存 -50%, 吞吐 +20-30% |
| Phase 5: Batch-3A | ❌ 未开始 | 0% | ⭐ 低 | 吞吐 +2× (负载不均衡场景) |
| Phase 6: Batch-3B | ❌ 未开始 | 0% | 可选 | 理论上限 |

---

## ❌ Phase 2: Batch-1 优化（未完成部分）

### ❌ P1: DWO 检测移出热路径

**优先级**: ⭐⭐ 中（边际收益）

**问题描述**:
- 当前实现在 `ExecuteConstraintCheck` 中每次约束检查都重算 `d_cur_dom_size` 并判空
- 热路径计算开销大，影响并发性能

**设计方案**: 见 [COMPREHENSIVE_DESIGN_V2.md §5.2](BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md#52-dwo-检测移出热路径--高优先级)

**实现要点**:
```cuda
// 1. 修改 ExecuteConstraintCheck：只做删位 + 标记 changed
__device__ PropagateResult ExecuteConstraintCheck_Optimized(
    int cid,
    WorldWorkspace& ws,
    const GModelData& model,
    u32* changed_vars_bitmap) {  // 新增参数

    // ... 计算新域 new_dom_x, new_dom_y ...

    if (new_dom_x != old_dom_x) {
        ws.bitDom[var_x_base + w] = new_dom_x;
        // 标记变量被修改（不立即判空）
        atomicOr(&changed_vars_bitmap[var_x / 32], 1u << (var_x % 32));
    }
    // var_y 同理...

    return r;  // 不返回 inconsistent
}

// 2. 在 RunGACToFixpoint_BlockSync 的迭代边界集中检查
__device__ bool CheckDWOAndUpdateSizes(
    WorldWorkspace* ws,
    u32* changed_vars_bitmap,
    int num_vars,
    int bit_dom_int_size) {

    __shared__ bool any_dwo;
    if (threadIdx.x == 0) any_dwo = false;
    __syncthreads();

    // 并行检查被修改的变量
    for (int v = threadIdx.x; v < num_vars; v += blockDim.x) {
        if (changed_vars_bitmap[v / 32] & (1u << (v % 32))) {
            // 重算域大小
            int new_size = 0;
            for (int w = 0; w < bit_dom_int_size; ++w) {
                new_size += __popc(ws->bitDom[v * bit_dom_int_size + w]);
            }
            ws->d_cur_dom_size[v] = new_size;

            if (new_size == 0) {
                any_dwo = true;  // 发现 DWO
            }
        }
    }
    __syncthreads();

    // 清空 changed 位图
    for (int w = threadIdx.x; w < (num_vars + 31) / 32; w += blockDim.x) {
        changed_vars_bitmap[w] = 0;
    }
    __syncthreads();

    return any_dwo;
}
```

**需要修改的文件**:
- `src/solver/gpu/GModel.cu`:
  - `ExecuteConstraintCheck_BpC_Workspace` (约 line 1513)
  - `RunGACToFixpoint_BlockSync` (约 line 1700)
- `include/solver/gpu/batch_probe_manager.h`:
  - `WorldWorkspace` 结构体（可能需要添加 changed_vars_bitmap）

**预期收益**:
- 减少热路径计算量（避免每次约束检查都 `__popc`）
- 更一致的并发行为（集中检查点）
- 预计吞吐提升 5-10%（取决于约束密度）

**实现难度**: ⭐⭐ 中等
- 需要修改核心传播逻辑
- 需要仔细测试正确性（DWO 检测不能遗漏）

**验收标准**:
- ✅ TIER0/TIER1 正确性 100%（删值一致性）
- ✅ 吞吐提升 5-10%
- ✅ GPU 利用率无明显下降

**状态**: ❌ 未实现

---

## ❌ Phase 3: Batch-2 基础版（未完成部分）

### ❓ 验收测试

**优先级**: ⭐⭐⭐ 最高（必须完成）

**任务**:
1. ✅ 运行 TIER0 完整回归（Batch-2 vs Batch-1 等价性）
2. ✅ 运行 TIER1 完整回归（Batch-2 vs Batch-1 等价性）
3. ✅ 对比 Batch-1 vs Batch-2 吞吐（probes/s）
4. ✅ 验证 Batch-2 precheck 开/关"结果一致/不崩溃"
5. ⚠️ Cheap Precheck 短路率：在 AC snapshot 前提下理论/实测 ~0%（更多是防御性检查；不作为吞吐收益来源）

**测试命令**:
```bash
# 单实例：ctest 回归（包含 precheck 开/关路径）
ctest --test-dir build --output-on-failure -R test_batch2_probe

# TIER0/TIER1：批量回归（Batch-2 Micro-Batch vs Batch-1）
python3 tests/python/compare_batch2_tier0.py --tier=0
python3 tests/python/compare_batch2_tier0.py --tier=1

# 吞吐对比
./build/benchmark_probe_throughput <instance.xml> --runs=3 --batch2_size=64
```

**预期结果**:
- ✅ TIER0: 12/12 通过（100%）
- ✅ TIER1: 39/39 通过（100%）
- ✅ Batch-2 吞吐 ≥ 5× Batch-1（实测：Queens-4 4.52x，Queens-12 **10.91x**）
- ✅ Precheck 早失败短路率 ~0%（AC snapshot 前提；仅作防御性检查）

**当前状态**: ✅ 已验收（正确性 + 吞吐）

---

### ✅ Stage 2: Persistent Blocks

**优先级**: ⭐⭐⭐ 高（核心主线）

**实现状态**: ✅ 完成（正确性验证通过，性能待优化）

**核心设计**:
- 单次 kernel launch，多个持久 blocks 通过 `atomicAdd(task_cursor)` 拉取任务
- 每个 block 拥有独立的 `WorldWorkspace`，包含 bitDom/d_cur_dom_size/frontier 等
- 消除 Stage 1 的多次 kernel launch 开销

**实现文件**:
- `include/solver/gpu/batch_probe_manager.h`: `Batch2PersistentControl`, `Batch2PersistentManager`
- `src/solver/gpu/GModel.cu`: `Batch2ProbeKernel_PersistentBlocks`
- `src/solver/gpu/batch_probe_manager.cu`: `Batch2PersistentManager` 类实现
- `tests/cpp/test_stage2_persistent.cpp`: Stage 1 vs Stage 2 对比测试

**测试结果** (2026-01-03 优化后):
| 实例 | 任务数 | Blocks | Stage 2 加速比 | 正确性 |
|------|--------|--------|----------------|--------|
| Queens-4 | 16 | 8 (auto) | **1.00x** | ✅ PASS |
| Queens-12 | 144 | 32 (auto) | 0.68x | ✅ PASS |
| Langford-3-9 | 405 | 32 (auto) | 0.85x | ✅ PASS |
| Graphw-05 | 1318 | 32 (auto) | **2.76x** | ✅ PASS |
| Rand-2-40-8 (t0.1) | 320 | 32 (auto) | **2.02x** | ✅ PASS |
| Rand-2-40-80 (t0.8) | 3200 | 32 (auto) | **1.76x** | ✅ PASS |

**已实现的优化**:
1. ✅ **自适应 num_blocks**：`ComputeOptimalNumBlocks(num_tasks, device_id)`
   - num_tasks <= 2*num_sms → num_sms blocks（如 queens-4 → 8 blocks）
   - 否则 4*num_sms blocks（如 queens-12 → 32 blocks）
   - 效果：queens-4 从 0.61x 提升到 **1.00x**
2. ✅ **可配置 chunk_size**：支持批量任务拉取（默认 chunk=1 最优）

**性能分析**:
- Stage 2 在高失败率/高约束密度场景表现优异（2-3x 加速）
- 均匀任务分布场景略慢于 Stage 1（0.68x-0.85x）

**验收标准**:
- ✅ 正确性：Stage 1 与 Stage 2 失败 probe 集合完全一致
- ✅ 性能：queens-4 达到 1.00x（与 Stage 1 持平）
- ✅ 多场景覆盖：graphw-05 2.76x, rand 问题 1.76x-2.02x

---

## ❌ Phase 4: Batch-2 优化版（未实现）

### ❌ 4.1 Copy-on-Write Delta 存储

**优先级**: ⭐⭐ 中（当前方案 A 已足够高效）

**何时需要**:
- B > 32（L2 缓存压力，Jetson Orin Nano 只有 1MB L2）
- 大问题（Queens-100+）
- 实测发现内存瓶颈时

**设计方案**: 见 [COMPREHENSIVE_DESIGN_V2.md §4.2](BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md#方案-bcopy-on-write-delta-存储内存优化)

**核心数据结构**:
```cpp
struct DeltaBlock {
    static constexpr int MAX_MODIFIED_VARS = 64;
    static constexpr int HASH_SIZE = 128;  // 必须是 2 的幂

    int probe_id;
    int num_modified;

    // O(1) 查找：var_id → slot 的 hash 表
    int hash_table[HASH_SIZE];  // -1 = 空

    // 实际数据
    int var_ids[MAX_MODIFIED_VARS];
    u32 domains[MAX_MODIFIED_VARS * MAX_DOM_WORDS];
    int dom_sizes[MAX_MODIFIED_VARS];

    // Frontier
    u32 frontier_A[MAX_BITMAP_WORDS];
    u32 frontier_B[MAX_BITMAP_WORDS];
};

// O(1) 查找实现（开放寻址）
__device__ int FindSlot(DeltaBlock& block, int var_id);
__device__ int AllocateSlot(DeltaBlock& block, int var_id);
```

**实现要点**:
1. 实现 `DeltaBlock` 结构体
2. 实现 O(1) hash 查找（开放寻址法）
3. 实现域访问接口（先查 delta，未找到则查 snapshot）
4. 修改 `ExecuteConstraintCheck` 使用 delta 存储
5. 实现 `ProbeMemoryPool` 内存池管理器

**需要修改的文件**:
- `include/solver/gpu/batch_probe_manager.h`: 添加 `DeltaBlock` 结构体
- `src/solver/gpu/GModel.cu`: 修改约束检查逻辑
- `src/solver/gpu/batch_probe_manager.cu`: 修改内存分配逻辑

**预期收益**:
- 内存占用减少 50-70%（取决于修改稀疏度）
- L2 缓存命中率提升（Queens-100: 3.3KB → ~800B per world）
- 支持更大的并发度 B（内存限制降低）

**实现难度**: ⭐⭐⭐ 高
- 复杂的数据结构（hash 表 + delta 存储）
- 需要仔细处理并发访问
- 需要完整测试（正确性 + 性能）

**验收标准**:
- ✅ TIER0/TIER1 正确性 100%
- ✅ 内存占用减少 ≥50%（Queens-100, B=64）
- ✅ 吞吐无明显下降（±5%）
- ✅ 大并发度（B=128）时吞吐提升 ≥20%

**状态**: ❌ 未实现

---

### ❌ 4.2 Warp-per-Word 约束检查

**优先级**: ⭐⭐ 中（可选优化）

**设计方案**: 见 [COMPREHENSIVE_DESIGN_V2.md §5.3](BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md#53-warp-per-word-约束检查--中优先级)

**核心思想**:
- 每个 warp（32 线程）处理一个 domain word（32 个值）
- 使用 `__ballot_sync()` 收集结果
- 减少 shared memory 原子操作

**实现框架**:
```cuda
__device__ void CheckConstraintWarpPerWord(
    int cid,
    WorldWorkspace& ws,
    const GModelData& model) {

    int warp_id = threadIdx.x / 32;
    int lane_id = threadIdx.x % 32;
    int num_warps = blockDim.x / 32;

    for (int word = warp_id; word < bit_dom_int_size; word += num_warps) {
        u32 dom_word = ws.bitDom[var_x * bit_dom_int_size + word];

        // lane i 处理 value = word*32 + i
        int value = word * 32 + lane_id;
        bool keep = false;

        if (dom_word & (1u << lane_id)) {
            // 检查该值是否有支持
            keep = HasSupport(cid, var_x, value, var_y, ws, model);
        }

        // warp 内收集结果
        u32 keep_mask = __ballot_sync(0xFFFFFFFF, keep);

        // lane 0 更新该 word
        if (lane_id == 0) {
            u32 new_word = dom_word & keep_mask;
            if (new_word != dom_word) {
                ws.bitDom[var_x * bit_dom_int_size + word] = new_word;
                // 标记变化...
            }
        }
    }
    __syncwarp();
}
```

**预期收益**:
- 减少 shared memory 原子操作
- 更好利用 warp 级原语（Ampere 优化）
- 预计吞吐提升 10-15%（约束密集问题）

**实现难度**: ⭐⭐⭐ 中高
- 需要重构约束检查逻辑
- 需要处理 warp 内协作

**状态**: ❌ 未实现

---

### ✅ 4.3 只读数据优化

**优先级**: ⭐⭐ 中（推荐实现）

**设计方案**: 见 [COMPREHENSIVE_DESIGN_V2.md §5.4](BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md#54-只读数据优化--中优先级)

#### 方案 A: Managed Memory + MemAdvise（推荐）

**适用数据**: `bitSupData`, `subscription`, `constraint_scopes`

**实现代码**:
```cpp
// 在 GModelAdapter::BuildGModel 或 GModel::InitializeGPUResources 中添加

void GModel::OptimizeManagedReadOnly() {
    int device_id = 0;  // 假设设备 0

    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, device_id);

    // 1. 设置只读提示
    cudaMemAdvise(bitSupData, bitSup_size,
                  cudaMemAdviseSetReadMostly, device_id);
    cudaMemAdvise(d_subscription, subscription_size,
                  cudaMemAdviseSetReadMostly, device_id);
    cudaMemAdvise(constraint_scopes, scopes_size,
                  cudaMemAdviseSetReadMostly, device_id);

    // 2. 检测是否需要 prefetch
    if (prop.concurrentManagedAccess == 0) {
        // Jetson UMA：跳过 prefetch
        LOG(INFO) << "Device uses integrated UMA, no prefetch needed";
        return;
    }

    // 3. 独立显存设备：prefetch 到 GPU
    cudaMemPrefetchAsync(bitSupData, bitSup_size, device_id);
    cudaMemPrefetchAsync(d_subscription, subscription_size, device_id);
    cudaMemPrefetchAsync(constraint_scopes, scopes_size, device_id);
}
```

**需要修改的文件**:
- `src/solver/gpu/GModel.cu`: 添加 `OptimizeManagedReadOnly()` 方法
- `src/model/gmodel_adapter.cu`: 在 `BuildGModel()` 末尾调用优化函数

**代码现状对齐**：
- `src/model/gmodel_adapter.cu` 已实现 `PrefetchBitDomToGPU()`，并对 Jetson（`concurrentManagedAccess == 0`）跳过 `cudaMemPrefetchAsync`。
- ✅ **2025-12-31 已实现**：新增 `GModelAdapter::OptimizeReadOnlyMemoryAdvice()` 方法（`gmodel_adapter.cu:466-526`），在 `Build()` 末尾自动调用（`gmodel_adapter.cu:330-331`）。
- 优化目标：`bitSupData`, `d_subscription`, `d_subscription_offset`, `constraint_scopes`

**预期收益**:
- 减少 10-20% 一致性开销（Jetson UMA）
- L2 缓存命中率提升（读密集访问）

**实现难度**: ⭐ 简单
- 只需添加几行代码
- 无需修改数据布局

**验收标准**:
- ✅ 编译通过，无运行时错误
- ✅ TIER0 正确性不变
- ❓ 吞吐提升 5-15%（bitSup 密集访问场景，待量化测试）

**状态**: ✅ 已实现（2025-12-31）

---

#### 方案 B: Device-Only + __ldg()（高级优化）

**优先级**: ⭐ 低（生产环境优化）

**实现要点**:
1. 迁移 `bitSupData` 到 device-only 内存
2. 在 kernel 中使用 `__ldg()` 内建函数
3. 保留 host 端副本用于 CPU 访问

**预期收益**:
- 最大化缓存效率（纹理缓存 + 只读缓存）
- 吞吐提升 20-30%

**缺点**:
- 实现复杂度高
- CPU 端无法直接访问（调试不便）

**状态**: ❌ 未实现

---

## ❌ Phase 5: Batch-3A 异步调度（未实现）

**优先级**: ⭐ 低（可选，适合负载不均衡场景）

**核心设计**: 见 [COMPREHENSIVE_DESIGN_V2.md §6](BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md#6-batch-3a异步调度--工作窃取)

**关键组件**:
1. ❌ 约束聚合任务队列 `(cid, world_mask)`
2. ❌ 持久线程块 + 约束亲和性
3. ❌ Shared Memory bitSup 缓存
4. ❌ 工作窃取机制（两级位图优化）

**适用场景**:
- 不同 probe 收敛速度差异大（负载不均衡）
- 需要极致吞吐（20-50× baseline）

**预期收益**:
- 吞吐提升 2× 以上（相比 Batch-2）
- GPU 利用率 80-90%

**实现难度**: ⭐⭐⭐⭐ 很高
- 复杂的任务队列管理
- 需要仔细调优（约束亲和性、工作窃取策略）

**状态**: ❌ 未实现

---

## ❌ Phase 6: Batch-3B World-SIMD（未实现）

**优先级**: 可选（研究方向）

**核心设计**: 见 [COMPREHENSIVE_DESIGN_V2.md §7](BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md#7-batch-3b-world-simd极致优化)

**关键创新**:
1. ❌ 域表示转置（`dom_mask[var][value]` 存储 world 位集）
2. ❌ 向量化约束检查（基于 bitSup 位集）

**预期收益**:
- 理论上限吞吐（约束密集问题达到 80%+ 峰值）
- 极致内存效率

**实现难度**: ⭐⭐⭐⭐⭐ 极高
- 需要完全重构数据布局
- 复杂的向量化逻辑

**状态**: ❌ 未实现

---

## 🎯 推荐实施优先级

基于当前状态和预期收益，建议按以下顺序实施：

### 第一优先级（立即执行）

1. **✅ Phase 3 验收测试**
   - 预计时间：1-2 天
   - 风险：低
   - 收益：验证 Batch-2 正确性和性能

2. **✅ Phase 4.3 只读数据优化（方案 A）**
   - 预计时间：半天
   - 风险：极低
   - 收益：5-15% 吞吐提升

### 第二优先级（根据测试结果决定）

3. **Phase 2 P1: DWO 检测延迟**
   - 预计时间：2-3 天
   - 风险：中（需要仔细测试正确性）
   - 收益：5-10% 吞吐提升

### 第三优先级（根据实际需求）

4. **Phase 4.1 Copy-on-Write**
   - 触发条件：发现内存瓶颈（B > 32 或 Queens-100+）
   - 预计时间：1 周
   - 收益：内存 -50%, 大并发度吞吐 +20-30%

5. **Phase 4.2 Warp-per-Word**
   - 触发条件：约束密集问题吞吐不达标
   - 预计时间：3-5 天
   - 收益：10-15% 吞吐提升

### 可选优先级（长期规划）

6. **Phase 5 Batch-3A**
   - 触发条件：负载严重不均衡
   - 预计时间：2 周

7. **Phase 6 Batch-3B**
   - 研究方向，暂不推荐

---

## 📝 跟踪记录

### 2026-01-03
- ✅ **Stage 2 性能优化完成**
  - 实现自适应 num_blocks（`ComputeOptimalNumBlocks`）
  - 添加 chunk_size 可配置支持（批量任务拉取）
  - queens-4：从 0.61x 提升到 **1.00x**（+64%）
  - rand-2-40-8：从 1.11x 提升到 **2.02x**（+82%）
  - graphw-05 保持 2.76x 加速
- ✅ **Auto Stage Selection 实现**
  - 新增 `AutoStageSelector` 类：基于采样统计自动选择 Stage 1/2
  - 决策因素：fail_rate、avg_deletions、num_tasks
  - 扩展 `benchmark_probe_throughput` 支持 Stage 1 vs Stage 2 对比和 `--auto` 模式
  - Stage 2 per-task 统计：记录每个任务的 iterations/deletions
- 📝 **结论**：Stage 2 在高失败率/高约束密度场景优于 Stage 1；Auto 选择器能正确区分两种场景

### 2026-01-02
- ✅ **Phase 3 Stage 2 Persistent Blocks 实现完成**
  - 新增 `Batch2PersistentControl` 控制结构和 `Batch2PersistentManager` 类
  - 实现 `Batch2ProbeKernel_PersistentBlocks` kernel（atomicAdd 任务拉取）
  - 新增 `test_stage2_persistent.cpp` 验证测试
- ✅ **正确性验证通过**：Queens-4/Queens-12 Stage 1 与 Stage 2 结果完全一致
- ⚠️ **性能待优化**：Stage 2 当前 0.64x 慢于 Stage 1
- 📝 **下一步**：Stage 2 性能优化 → ✅ 2026-01-03 已完成

### 2025-12-31
- ✅ Phase 1 完成
- ✅ Phase 2 P0-1/P0-2 完成（Frontier 策略化 + Cheap Precheck）
- ✅ Phase 3 核心实现完成（Batch2ProbeManager + Kernel）
- ❌ Phase 2 P1 未实现（DWO 检测延迟）
- ✅ Phase 3 验收测试已完成（TIER0 12/12，TIER1 39/39）
- ✅ **Phase 3 吞吐基准已完成**：新增 `benchmark_probe_throughput` 工具
  - Queens-4 (16 probes): Batch-1 2,440 p/s → Batch-2 11,020 p/s（**4.52x 加速**）
  - Queens-12 (144 probes): Batch-1 2,261 p/s → Batch-2 24,674 p/s（**10.91x 加速**）
- ✅ **Phase 4.3 只读数据优化已实现**：新增 `GModelAdapter::OptimizeReadOnlyMemoryAdvice()`，对 `bitSupData/d_subscription/d_subscription_offset/constraint_scopes` 设置 `cudaMemAdviseSetReadMostly`
- 📝 创建本文档，标记未完成工作
- 📝 **下一步**：Phase 3 Stage 2 - Persistent Blocks（核心主线）→ ✅ 2026-01-02 已完成

---

## 🔗 相关文档

- [BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md](BATCH_AC_GPU_COMPREHENSIVE_DESIGN_V2.md) - 完整设计文档
- [BATCH_AC_GPU_BATCH2_BATCH3_DESIGN.md](BATCH_AC_GPU_BATCH2_BATCH3_DESIGN.md) - Batch-2/3 原始设计
- [Batch_AC.md](Batch_AC.md) - SAC 理论基础

---

*文档版本: v1.0*
*维护者: CPIM 团队*
