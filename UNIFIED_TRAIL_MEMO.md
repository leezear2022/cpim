# 统一 CPU/GPU Trail 系统 - 实施备忘

**创建时间**: 2025-12-20
**目标**: 3.5 周内实现统一 CPU/GPU Trail,性能提升 30-50%,内存节省 90%

---

## 核心设计

### 原理

- **问题**: 当前全量域拷贝 (N=100, depth=20 → 1.56MB 浪费)
- **方案**: 增量 Trail + 前向覆盖 (O(1) 回溯,仅记录修改)
- **关键**: CPU/GPU 使用相同数据结构,支持自适应切换

### 零拷贝统一内存

```cpp
// 所有关键数据使用 cudaMallocManaged (Jetson 优势)
u32* bitDom;           // CPU/GPU 共享,无需同步
TrailEntry* trail_;    // 统一 Trail 结构
```

### O(1) 回溯

```cpp
void BacktrackTo(int level) {
  current_level = level;  // 仅指针移动!
  // 数据不恢复,由后续搜索覆盖
}
```

---

## 实施阶段

### Phase 1.1: CPU Trail 基础 (1 周) ← 当前阶段

**新增文件**:
- `include/base/unified_trail.h` - Trail 类定义
- `src/base/unified_trail.cpp` - Trail 实现

**修改文件**:
- [Network.h](include/Network.h) - IntVar 删除多级域,添加 `UnifiedTrail* trail_`
- [Network.cpp](src/Network.cpp:372-373) - 使用 `cudaMallocManaged`,调用 `trail_->RecordDomainChange()`
- [MAC.cpp](src/MAC.cpp:120) - 使用 `trail_->NewLevel()`/`BacktrackTo()`

**验收**: `python3 samples/batch_test_v2.py --tier=0` 100% 通过

---

### Phase 1.2: GPU Trail 集成 (1 周)

**修改文件**:
- [GModel.cuh](include/GModel.cuh:93-99) - 删除多级 bitDom,改为单层
- [GModel.cu](src/GModel.cu:446) - GPU kernel 使用原子操作记录 Trail

**验收**: GPU 模式 TIER0 测试通过

---

### Phase 1.3: 自适应引擎 (1 周)

**新增文件**:
- `include/solver/adaptive_engine.h` - 自适应传播引擎
- `src/solver/adaptive_engine.cpp` - 实现

**功能**: 根据工作负载自动选择 CPU/GPU AC/SAC

**验收**: 混合规模测试集,性能优于固定后端

---

### Phase 1.4: SAC 统一 (0.5 周)

**实现**: CPU/GPU SAC 使用临时缓冲区 (2× 单层域,而非 20× 多级域)

**验收**: SAC 预处理正确性验证

---

## 性能预期

| 指标 | 当前 | 目标 | 提升 |
|------|------|------|------|
| 内存占用 | 1.56 MB | 118 KB | **92% ↓** |
| 回溯速度 | O(N×D) 拷贝 | O(1) 指针移动 | **100-500× ↑** |
| Queens-12 | 0.5s | 0.3s | **40% ↑** |

---

## 关键代码模式

### Trail 记录 (CPU/GPU 统一)

```cpp
// CPU 中
void IntVar::RemoveValue(int value) {
  int word_idx = value / BITSIZE;
  u32 old_bits = bit_doms_[word_idx];

  trail_->RecordDomainChange(id_, word_idx, old_bits);  // 记录
  bit_doms_[word_idx] &= ~(1u << (value % BITSIZE));    // 修改
}

// GPU 中
__device__ void RemoveValue_GPU(...) {
  int idx = atomicAdd(trail_size, 1);
  trail[idx] = {DOMAIN_CHANGE, {}, var_id, word_idx, old_bits};  // 记录
  atomicAnd(&bitDom[offset], ~mask);                              // 修改
}
```

### SAC 临时缓冲区

```cpp
// 不再需要多级域!
u32* temp_bitdom_;  // 仅 1× 单层域大小

// SAC 测试模式
CopyCurrentDomain(temp_bitdom_);         // 备份
trail_->NewLevel();
engine_->Propagate(level);               // 可自适应调用 CPU/GPU!
trail_->BacktrackTo(level - 1);
RestoreDomain(temp_bitdom_);             // 恢复
```

---

## 快速参考

### 验证命令

```bash
# 构建
cd build && cmake .. && make -j4

# 单例测试
./cpim_test_parser --input=../samples/bench/queens-4_ext.xml

# 回归测试
python3 ../samples/batch_test_v2.py --tier=0

# 内存检查
valgrind --leak-check=full ./cpim_test_parser --input=../samples/bench/queens-8.xml
```

### 调试技巧

```cpp
// Trail 内容打印
void UnifiedTrail::DebugPrint() {
  LOG(INFO) << "Trail size: " << trail_size_
            << ", Level: " << current_level_;
  for (int i = 0; i < trail_size_; ++i) {
    auto& e = trail_entries_[i];
    LOG(INFO) << "  [" << i << "] var=" << e.var_id
              << " word=" << e.word_index
              << " old=" << std::hex << e.old_bits;
  }
}
```

---

## 风险缓解

| 风险 | 应对 |
|------|------|
| Trail 实现 Bug | 1. 每步验证 TIER0<br>2. 对比旧版结果<br>3. 添加单元测试 |
| 性能回退 | 1. Google Benchmark 监控<br>2. Profiler 定位瓶颈 |
| GPU 集成问题 | 1. 先完成 CPU 版本<br>2. 渐进式集成 GPU |

---

## 完整计划

详见: `/home/lee/.claude/plans/floofy-strolling-charm.md`
