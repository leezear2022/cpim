# SAC 系列算法在 Unified Trail 架构下的高效实施设计

**日期**: 2025-12-23
**状态**: 草案
**目标**: 在 `cpim` 现有的 `UnifiedTrail` 架构下，设计高效的 Singleton Arc Consistency (SAC) 算法实现方案。

---

## 1. 背景与挑战

Singleton Arc Consistency (SAC) 是一种强一致性技术。它尝试将变量 $X$ 赋值为 $v$ ($X=v$)，然后运行 Arc Consistency (AC)。
- 如果 AC 导致冲突（Domain Wipeout, DWO），则 $v$ 不是 SAC-Consistent，可以从 $D(X)$ 中**永久删除**。
- 如果 AC 成功，则恢复状态，尝试下一个值。

**主要挑战**:
1.  **频繁的回溯**: SAC 需要对每个变量的每个值进行“试错”。这意味着级数级的 `Assign` -> `Propagate` -> `Backtrack` 循环。
2.  **Trail 开销**: 在标准 Trail 机制下，每次 Probe 内部的 AC 可能移除数千个值，这些都需要记录到 Trail 并在回溯时恢复。虽然 `UnifiedTrail` 是 $O(1)$ 指针回溯，但**记录过程**（内存写入）和**恢复过程**（内存读取+写入）仍有带宽开销。
3.  **GPU 延迟**: 在 GPU 上，频繁启动微小的 Kernel (Single Probe) 会被 Launch Latency (5-10us) 淹没。

---

## 2. 核心架构设计

我们提供两种实现路径，分别针对不同的场景和数据规模。

### 方案 A: 标准 Trail-based SAC (通用，易实现)

利用现有 `UnifiedTrail` 接口。适用于大多数中等规模问题或作为基准实现。

**伪代码逻辑**:
```cpp
void EnforceSAC1(GModel* model) {
    bool changed = true;
    while (changed) {
        changed = false;
        // 遍历所有变量
        for (int var : model->vars) {
            if (model->IsAssigned(var)) continue;
            
            // 遍历域中所有值 (snapshot domain to iterate safely)
            std::vector<int> values = model->GetDomainValues(var);
            
            for (int val : values) {
                // 1. 记录当前层级 (Level L)
                int base_level = trail->CurrentLevel();
                
                // 2. 进入探测层 (Level L+1)
                trail->NewLevel();
                
                // 3. 试探赋值
                model->AssignValue(var, val);
                
                // 4. 传播 (GAC)
                bool consistent = engine->Propagate();
                
                // 5. 回溯 (恢复到 Level L)
                // 这一步利用 UnifiedTrail 的 O(1) 指针回溯
                // 并通过 trail 反向遍历恢复被删除的值
                trail->BacktrackTo(base_level);
                
                // 6. 剃刀处理 (Shaving)
                if (!consistent) {
                    // 假如试探失败，说明 val 在 Level L 是不可能的
                    // 关键：在 Level L 删除 val
                    // 这会自动触发 Level L 的 Trail 记录和传播队列更新
                    model->RemoveValue(var, val);
                    
                    // 立即在当前层 (Level L) 传播，尽早修剪
                    if (!engine->Propagate()) {
                        return; // 全局失败
                    }
                    changed = true;
                    
                    // 优化：如果 var 被剃光了，无需继续试探其他值
                    if (model->GetDomainSize(var) == 0) return;
                }
            }
        }
    }
}
```

**优点**:
- **代码简洁**: 完全复用现有的 `UnifiedTrail` 逻辑。
- **正确性保证**: Trail 自动处理复杂的域恢复逻辑。

**缺点**:
- **Trail 污染**: Probe 过程中产生的大量临时 `TrailEntry` 会占用内存带宽，即使它们马上就会被丢弃。

---

### 方案 B: Snapshot-based SAC (高性能，Phase 1.4 推荐)

针对 SAC 这种“浅层但密集”的探测，**全量快照 (Snapshot)** 往往比 **增量记录 (Trail)** 更快。
因为 SAC 的 Probe 深度仅为 1，我们可以直接备份当前的域状态 (`memcpy`)，做完 AC 后直接覆盖恢复，完全绕过 Trail 系统。

**前提**: 需要 `GModel` 支持 `SaveState` 和 `LoadState` (即 `memcpy` `bitDom`)，以及一个开关来**禁用 Trail 记录**。

**伪代码逻辑**:
```cpp
void EnforceSAC_Optimized(GModel* model) {
    // 1. 分配临时缓冲区 (仅需 1x bitDom 大小)
    // 利用 Jetson 统一内存，GPU 也可以访问
    u32* snapshot_buffer; 
    cudaMallocManaged(&snapshot_buffer, model->total_bitdom_bytes);
    
    bool changed = true;
    while (changed) {
        changed = false;
        for (int var : unassigned_vars) {
            // 优化：检查是否值得做 SAC (如域大小 < 阈值)
            if (model->d_cur_dom_size[var] > SAC_LIMIT) continue;

            std::vector<int> values = model->GetDomainValues(var);
            for (int val : values) {
                
                // === 关键优化开始 ===
                
                // A. 快照：备份当前所有变量的域
                // Jetson 上这是整块内存拷贝，带宽很高
                memcpy(snapshot_buffer, model->bitDom, model->total_bitdom_bytes);
                
                // B. 禁用 Trail 记录
                // 告诉底层 IntVar / GModel 不要写 TrailEntry
                model->SetTrailRecording(false);
                
                // C. 试探与传播
                model->AssignValue(var, val); // 不写 Trail
                bool consistent = engine->Propagate(); // GAC 过程中的删除也不写 Trail
                
                // D. 恢复状态
                // 直接内存覆盖，瞬间还原
                memcpy(model->bitDom, snapshot_buffer, model->total_bitdom_bytes);
                
                // E. 重新启用 Trail
                model->SetTrailRecording(true);
                
                // === 关键优化结束 ===
                
                if (!consistent) {
                    // 在正式模式下删除值 (会写 Trail)
                    model->RemoveValue(var, val);
                    
                    // 在正式模式下传播
                    if (!engine->Propagate()) return;
                    changed = true;
                }
            }
        }
    }
}
```

**性能权衡**:
- **Snapshot 优势**: 当单次 Probe 导致的域缩减量很大（>5%）时，`memcpy` 比写几千个 `TrailEntry` 更快，且避免了 `Trail` 扩容风险。
- **Snapshot 劣势**: 当域非常巨大（如 100MB）且 AC 几乎不剪枝时，`memcpy` 开销大。
- **结论**: 对于 XCSP3 大多数实例（域较小，约束紧还是紧），**Snapshot 方案通常是 SAC 的最优解**。这也符合 `UNIFIED_TRAIL_MEMO.md` Phase 1.4 的规划。

---

## 3. 实现细节与数据结构

为了支持方案 B，我们需要在现有的类中添加以下接口：

### 3.1 `UnifiedTrail` 修改
无需修改，但需要被“绕过”。

### 3.2 `GModel` 修改
```cpp
class GModel {
public:
    // 开关 Trail 记录
    void SetTrailRecording(bool enable) {
        trail_recording_enabled_ = enable;
    }

    // 修改 RemoveValue 逻辑
    bool RemoveValue(int var, int val) {
        // ... 计算 index ...
        if (trail_recording_enabled_ && trail_) {
            trail_->RecordDomainChange(...);
        }
        // ... 执行位操作 ...
    }
    
    // 专门的快照接口
    void SnapshotDomains(u32* dest) {
        cudaMemcpy(dest, bitDom, bytes, cudaMemcpyDefault);
    }
    
    void RestoreDomains(const u32* src) {
        cudaMemcpy(bitDom, src, bytes, cudaMemcpyDefault);
        // 注意：还需要恢复 d_cur_dom_size 等元数据
        // 或者在 restore 后重新计算 dom_size (GPU并行计算很快)
    }

private:
   bool trail_recording_enabled_ = true;
};
```

---

## 4. 进阶：GPU-Native SAC (未来展望)

在 Jetson 平台上，真正的极致性能来自于 **Block-Parallel SAC**。
- **思路**: 不再是一个一个值试探。
- **调度**: 启动 $K$ 个 CUDA Thread Blocks。
- **数据**: 每个 Block 拥有一份 `bitDom` 的私有拷贝（在 Shared Memory 或 Global Memory 临时区）。
- **执行**: 
    - Block $i$ 负责试探 $Var_X = Val_i$。
    - Block $i$ 独立运行 GAC。
    - Block $i$ 报告结果（Consistent / Inconsistent）。
- **汇总**: CPU 根据 GPU 汇报的 bitmap 结果，批量删除无效值。

这需要大量的显存（$N \times \text{SizeOf(Model)}$），但在 Unified Memory 架构下，可以利用系统内存作为后备，是未来 Phase 2.0 的重点优化方向。

---

## 5. 实施计划

1.  **Refactor**: 在 `GModel` 中为了 SAC 添加 `SetTrailRecording(bool)` 开关。
2.  **Implementation**: 实现 `SAC1` 类，默认为 Snapshot 模式（因为 `GModel` 数据结构扁平，非常适合 memcpy）。
3.  **Verification**: 使用 `queens` 等强约束问题验证 SAC 的剪枝能力是否强于标准 GAC。
