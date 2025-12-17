# GModel 代码结构文档

> **版本**: 2025-12-09
> **状态**: 已实现持久化 Kernel (Cooperative Groups)

---

## 1. 整体架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           GModel 架构图                                 │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌────────────────────────────────────────────────────────────────────┐ │
│  │                    GModelSolver / SimpleGModelSolver               │ │
│  │  (CPU 端 MAC 搜索引擎)                                             │ │
│  │  ┌──────────────────────────────────────────────────────────────┐  │ │
│  │  │ Solve()                                                      │  │ │
│  │  │   ├─ EnforceGAC() / EnforceGAC_Persistent()                  │  │ │
│  │  │   └─ Search()                                                │  │ │
│  │  │       ├─ SelectVariable()      最小域启发式                  │  │ │
│  │  │       ├─ CreateNewLevel()      层级管理                      │  │ │
│  │  │       ├─ AssignValue()         赋值                          │  │ │
│  │  │       ├─ EnforceGAC()          约束传播                      │  │ │
│  │  │       └─ BackToLevel()         回溯                          │  │ │
│  │  └──────────────────────────────────────────────────────────────┘  │ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                    │                                    │
│                                    ▼                                    │
│  ┌────────────────────────────────────────────────────────────────────┐ │
│  │                         GModel (GPU 模型)                          │ │
│  │  ┌────────────────────────────────────────────────────────────┐    │ │
│  │  │ 数据存储 (Unified Memory)                                  │    │ │
│  │  │   ├─ bitDom[level][var][word]     多层级域位集             │    │ │
│  │  │   ├─ bitSupData[cid][val][word]   支持表位集               │    │ │
│  │  │   ├─ d_cur_dom_size[level][var]   域大小缓存               │    │ │
│  │  │   ├─ constraint_scopes[cid]       约束作用域               │    │ │
│  │  │   └─ d_subscription[var]          变量邻接约束 (CSR)       │    │ │
│  │  └────────────────────────────────────────────────────────────┘    │ │
│  │  ┌────────────────────────────────────────────────────────────┐    │ │
│  │  │ GAC 传播引擎                                               │    │ │
│  │  │   ├─ EnforceGAC()            Host 迭代 + GPU Kernel        │    │ │
│  │  │   └─ EnforceGAC_Persistent() GPU 内部迭代 (grid.sync)      │    │ │
│  │  └────────────────────────────────────────────────────────────┘    │ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                    │                                    │
│                                    ▼                                    │
│  ┌────────────────────────────────────────────────────────────────────┐ │
│  │                      GPU Kernels                                   │ │
│  │  ┌─────────────────────┐  ┌─────────────────────────────────────┐  │ │
│  │  │ BitmapGACKernel     │  │ PersistentGACKernel                 │  │ │
│  │  │ (普通模式)          │  │ (持久化模式, Cooperative Groups)    │  │ │
│  │  │                     │  │                                     │  │ │
│  │  │ Host 控制迭代       │  │ GPU 内部控制迭代                    │  │ │
│  │  │ while(!done) {      │  │ for(iter) {                         │  │ │
│  │  │   Kernel<<<>>>      │  │   process frontier                  │  │ │
│  │  │   cudaSync()        │  │   grid.sync()                       │  │ │
│  │  │   check bitmap      │  │   check convergence                 │  │ │
│  │  │   swap(A,B)         │  │   swap(A,B)                         │  │ │
│  │  │ }                   │  │   grid.sync()                       │  │ │
│  │  │                     │  │ }                                   │  │ │
│  │  └─────────────────────┘  └─────────────────────────────────────┘  │ │
│  └────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. 文件结构

```
cpim/
├── include/
│   └── GModel.cuh              # GModel 类定义、控制结构
│
├── src/
│   ├── GModel.cu               # GModel 实现（Kernel + Host 方法）
│   └── GModelSolver.cu         # (可选) 完整 MAC 求解器
│
├── test/
│   └── gmodel_solver.cpp       # 简单 MAC 测试程序
│
└── aig_docs/
    ├── GMODEL_CODE_STRUCTURE.md       # 本文档
    └── GMODEL_OPTIMIZATION_MEMO.md    # 优化备忘
```

---

## 3. 核心数据结构

### 3.1 GModel 类 (`include/GModel.cuh`)

```cpp
class GModel {
 public:
  // ========== 模型维度 (const) ==========
  const int num_vars;           // 变量数量
  const int num_constraints;    // 约束数量
  const int max_dom_size;       // 最大域大小
  const int bit_dom_int_size;   // 每个域需要的 uint32 数量
  const int bit_doms_int_size;  // 所有域需要的 uint32 数量
  const int max_depth;          // 最大搜索深度

  // ========== 域存储 (Unified Memory) ==========
  u32* bitDom;                  // 多层级域位集
  int* d_cur_dom_size;          // 域大小缓存

  // ========== 支持表 ==========
  cudaTextureObject_t texObj_BitSup;  // 纹理对象（只读优化）
  uint2* bitSupData;                   // 原始数据（Unified Memory）

  // ========== 约束信息 ==========
  int2* constraint_scopes;       // 约束作用域 (x, y)
  uint3* d_subscription;         // 变量邻接约束 (CSR 格式)
  int* d_subscription_offset;    // CSR 偏移数组

  // ========== GAC 控制资源 ==========
  GACControl* d_gac_control;              // 普通 GAC 控制块
  PersistentGACControl* d_persistent_control;  // 持久化 GAC 控制块
  u32* d_queue_bitmap_A;                  // Current Frontier
  u32* d_queue_bitmap_B;                  // Next Frontier

  // ========== 核心方法 ==========
  GacStats EnforceGAC(bool verbose, int assigned_var);           // 普通 GAC
  GacStats EnforceGAC_Persistent(bool verbose, int assigned_var); // 持久化 GAC

  int CreateNewLevel();           // 创建新搜索层级
  void BackToLevel(int level);    // 回溯到指定层级
  bool AssignValue(int var, int value, int level);  // 赋值
};
```

### 3.2 控制结构

```cpp
// 普通 GAC 控制结构
struct GACControl {
  int inconsistent_flag;         // 全局不一致标志
  int scanner_index;             // Bitmap 扫描游标
  unsigned long long deletions;  // 累计删值数
  int iterations;                // 传播轮数
};

// 持久化 Kernel 控制结构 (扩展)
struct PersistentGACControl {
  // 基础字段（与 GACControl 兼容）
  int inconsistent_flag;
  int scanner_index;
  unsigned long long deletions;
  int iterations;

  // 持久化专用字段
  int converged_flag;            // 收敛标志
  int frontier_nonempty;         // 下轮 frontier 非空标志
  u32* frontier_A;               // Current Frontier 指针
  u32* frontier_B;               // Next Frontier 指针
};
```

---

## 4. GAC 传播流程

### 4.1 普通模式 (`EnforceGAC`)

```
┌─────────────────────────────────────────────────────────────────┐
│                    Host 端 (CPU)                                │
│  while (!done) {                                                │
│    1. scanner_index = 0                                         │
│    2. BitmapGACKernel<<<blocks, threads>>>                      │
│    3. cudaDeviceSynchronize()          ← 同步等待               │
│    4. if (inconsistent_flag) done = true                        │
│    5. if (bitmap_B 为空) done = true                            │
│    6. swap(bitmap_A, bitmap_B)                                  │
│    7. memset(bitmap_B, 0)                                       │
│  }                                                              │
└─────────────────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────────────┐
│                    GPU 端 (BitmapGACKernel)                     │
│  while (true) {                                                 │
│    cid = FetchNextCidFromBitmap(frontier_cur)                   │
│    if (cid < 0) break                                           │
│                                                                 │
│    result = ExecuteConstraintCheck_BpC(cid)                     │
│    if (result.deletions > 0) {                                  │
│      PropagateVarToNextBitmap(x, frontier_next)                 │
│      PropagateVarToNextBitmap(y, frontier_next)                 │
│    }                                                            │
│    if (result.inconsistent) atomicExch(inconsistent_flag, 1)    │
│  }                                                              │
└─────────────────────────────────────────────────────────────────┘
```

**特点**：
- 每轮传播需要 Kernel 启动开销 (~5-15μs)
- Host 端控制循环，可以灵活处理
- 适合大规模传播（启动开销占比小）

### 4.2 持久化模式 (`EnforceGAC_Persistent`)

```
┌─────────────────────────────────────────────────────────────────┐
│                    Host 端 (CPU)                                │
│  1. 初始化 d_persistent_control                                 │
│  2. 初始化 frontier_A (激活约束)                                │
│  3. cudaLaunchCooperativeKernel(PersistentGACKernel)            │
│  4. cudaDeviceSynchronize()           ← 只同步一次              │
│  5. 读取结果                                                    │
└─────────────────────────────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────────────┐
│                GPU 端 (PersistentGACKernel)                     │
│                                                                 │
│  cg::grid_group grid = cg::this_grid();                         │
│                                                                 │
│  for (iter = 0; iter < max_iterations; ++iter) {                │
│                                                                 │
│    // 阶段 1: 重置扫描索引                                       │
│    if (blockIdx.x == 0 && threadIdx.x == 0)                     │
│      scanner_index = 0;                                         │
│    grid.sync();                                                 │
│                                                                 │
│    // 阶段 2: 处理当前 frontier                                  │
│    while ((cid = FetchNextCid()) >= 0) {                        │
│      ExecuteConstraintCheck(cid);                               │
│      PropagateToNextBitmap();                                   │
│    }                                                            │
│    grid.sync();  ← GPU 内部全局同步                              │
│                                                                 │
│    // 阶段 3: 检查收敛                                           │
│    if (inconsistent || frontier_B 为空) break;                  │
│                                                                 │
│    // 阶段 4: Swap 双缓冲                                        │
│    swap(frontier_A, frontier_B);                                │
│    grid.sync();                                                 │
│                                                                 │
│    // 阶段 5: 清空 next frontier                                 │
│    clear(frontier_B);                                           │
│    grid.sync();                                                 │
│  }                                                              │
└─────────────────────────────────────────────────────────────────┘
```

**特点**：
- 只启动一次 Kernel，消除多轮启动开销
- 使用 Cooperative Groups 的 `grid.sync()` 实现全局同步
- 适合多轮迭代的传播（10+ 轮时效果明显）

---

## 5. 关键 Kernel 函数

### 5.1 约束获取 (`FetchNextCidFromBitmap`)

```cpp
__device__ int FetchNextCidFromBitmap(
    u32* frontier_cur,
    int bitmap_size_words,
    int* scanner_index,
    int& local_word,      // 线程局部缓存
    int& local_offset) {

  while (true) {
    // 先检查局部缓存
    if (local_word != 0) {
      int bit = __ffs(local_word) - 1;
      local_word &= ~(1u << bit);
      return local_offset + bit;
    }

    // 原子获取下一个 word
    int w = atomicAdd(scanner_index, 1);
    if (w >= bitmap_size_words) return -1;

    // 原子交换清零
    u32 word = atomicExch(&frontier_cur[w], 0u);
    if (word == 0u) continue;

    local_word = word;
    local_offset = w * 32;
  }
}
```

### 5.2 约束检查 (`ExecuteConstraintCheck_BpC`)

```cpp
__device__ PropagateResult ExecuteConstraintCheck_BpC(
    int cid,
    const GModelData& model,
    int current_level,
    u32* shmem) {

  // 加载变量域到共享内存
  const int2 scope = model.constraint_scopes[cid];
  u32* s_dom_x = shmem;
  u32* s_dom_y = shmem + model.bit_dom_int_size;

  // 每个线程处理一个值
  int val = threadIdx.x;
  if (val < model.max_dom_size) {
    int word = val / 32, bit = val % 32;

    // 检查 x → y 的支持
    u32 sup_x = ...; // 从 bitSup 读取
    bool has_support_x = (sup_x & s_dom_y[...]) != 0;
    if (!has_support_x) {
      atomicAnd(&s_dom_x[word], ~(1u << bit));  // 删值
    }

    // 检查 y → x 的支持
    // ... 类似逻辑 ...
  }

  // 写回全局内存，统计删值数量
  // ...
}
```

### 5.3 邻接约束传播 (`PropagateVarToNextBitmap`)

```cpp
__device__ void PropagateVarToNextBitmap(
    int var,
    const GModelData& model,
    u32* next_bitmap) {

  // 遍历变量的邻接约束 (CSR 格式)
  const int start = model.d_subscription_offset[var];
  const int end = model.d_subscription_offset[var + 1];

  for (int i = start; i < end; ++i) {
    int cid = model.d_subscription[i].z;
    int w = cid / 32;
    int b = cid % 32;
    atomicOr(&next_bitmap[w], 1u << b);  // Bitmap 原子设置
  }
}
```

---

## 6. 多层级搜索支持

### 6.1 层级布局

```
bitDom 内存布局:
┌─────────────────────────────────────────────────────────────────┐
│ Level 0 │ Level 1 │ Level 2 │ ... │ Level (max_depth-1)         │
├─────────┼─────────┼─────────┼─────┼─────────────────────────────┤
│ var0    │ var0    │ var0    │     │                             │
│ var1    │ var1    │ var1    │     │                             │
│ ...     │ ...     │ ...     │     │                             │
│ var(n-1)│ var(n-1)│ var(n-1)│     │                             │
└─────────┴─────────┴─────────┴─────┴─────────────────────────────┘

索引计算:
  bitDom[level * bit_doms_int_size + var * bit_dom_int_size + word]
```

### 6.2 层级操作

```cpp
// 创建新层级（复制上一层域）
int CreateNewLevel() {
  ++current_level_;
  cudaMemcpy(dst, src, size, cudaMemcpyDeviceToDevice);
  return current_level_;
}

// 回溯（只需修改指针，不需要恢复数据）
void BackToLevel(int level) {
  current_level_ = level;
}
```

---

## 7. 使用示例

### 7.1 基本使用

```cpp
#include "GModel.cuh"
#include "model/gmodel_adapter.h"

// 从 IntermediateModel 构建
GModel gmodel = GModelAdapter::Build(im_model, options);

// 初始 GAC
GacStats stats = gmodel.EnforceGAC(true);  // verbose=true

// 搜索
int level = gmodel.CreateNewLevel();
gmodel.AssignValue(var, value, level);
stats = gmodel.EnforceGAC(false, var);  // 增量 GAC

// 回溯
gmodel.BackToLevel(prev_level);
```

### 7.2 使用持久化 Kernel

```cpp
// 持久化模式（需要 Cooperative Groups 支持）
GacStats stats = gmodel.EnforceGAC_Persistent(true);

// 如果设备不支持，会自动回退到普通模式
```

### 7.3 命令行测试

```bash
# 普通模式
./gmodel_solver ../samples/bench/queens-12_ext.xml

# 持久化模式
./gmodel_solver ../samples/bench/queens-12_ext.xml --persistent
```

---

## 8. 性能特点

| 特性 | 普通模式 | 持久化模式 |
|------|----------|------------|
| Kernel 启动次数 | N 次（N = 迭代轮数） | 1 次 |
| 同步机制 | `cudaDeviceSynchronize` | `grid.sync()` |
| 启动开销 | ~5-15μs/轮 | ~5-15μs 总计 |
| 适用场景 | 大规模传播 | 多轮迭代传播 |
| Cooperative Groups | 不需要 | 需要 |
| 回退机制 | N/A | 自动回退到普通模式 |

---

## 9. 依赖关系

```
GModel.cuh
    ↑
    │
GModel.cu ────────────────┐
    │                     │
    ├─ BitmapGACKernel    │
    ├─ PersistentGACKernel│
    ├─ EnforceGAC         │
    └─ EnforceGAC_Persistent
                          │
                          ▼
              cooperative_groups.h
              cuda_runtime.h
```

---

## 10. 未来优化方向

1. **Hybrid S (CPU Fallback)**: 小任务时跳过 GPU，直接用 CPU 处理
2. **Warp-level 优化**: 使用 Warp Shuffle 减少共享内存访问
3. **Stream 并行**: 多 Stream 并发处理不同约束组
4. **动态负载均衡**: 根据约束复杂度动态调整 block 分配

---

## 参考文档

- [GMODEL_OPTIMIZATION_MEMO.md](GMODEL_OPTIMIZATION_MEMO.md) - 优化备忘
- [CP_SCHEME_RECOMMENDATION.md](cp_schemes/CP_SCHEME_RECOMMENDATION.md) - 方案推荐
- [CLAUDE.md](../CLAUDE.md) - 项目开发指南
