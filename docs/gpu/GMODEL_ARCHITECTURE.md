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
│    // 阶段 1: 重置扫描索引                                      │
│    if (blockIdx.x == 0 && threadIdx.x == 0)                     │
│      scanner_index = 0;                                         │
│    grid.sync();                                                 │
│                                                                 │
│    // 阶段 2: 处理当前 frontier                                 │
│    while ((cid = FetchNextCid()) >= 0) {                        │
│      ExecuteConstraintCheck(cid);                               │
│      PropagateToNextBitmap();                                   │
│    }                                                            │
│    grid.sync();  ← GPU 内部全局同步                             │
│                                                                 │
│    // 阶段 3: 检查收敛                                          │
│    if (inconsistent || frontier_B 为空) break;                  │
│                                                                 │
│    // 阶段 4: Swap 双缓冲                                       │
│    swap(frontier_A, frontier_B);                                │
│    grid.sync();                                                 │
│                                                                 │
│    // 阶段 5: 清空 next frontier                                │
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

## 11. GAC Bit 编码的数学表示

> 本节从数学角度刻画 `bitDom` / `bitSupData` / frontier bitmap 这套 GAC 编码，与前文的代码结构一一对应，方便之后对齐 CPU/GPU 语义、对比实现。

### 11.1 域的 bit 编码

- 变量集合：\(V = \{X_1, \dots, X_n\}\)
- 最大域大小：\(M = \texttt{max\_dom\_size}\)
- 统一值编号集合：\(\mathcal{U} = \{0,1,\dots,M-1\}\)

在搜索层级 \(\ell\) 上，变量 \(X_i\) 的当前域是一个集合：

$$
D_i^{(\ell)} \subseteq \mathcal{U}
$$

用 bit 向量编码为：

$$
\mathbf{d}_i^{(\ell)} \in \{0,1\}^M, \quad
\mathbf{d}_i^{(\ell)}[v] =
\begin{cases}
1, & v \in D_i^{(\ell)} \\
0, & v \notin D_i^{(\ell)}
\end{cases}
$$

与内存布局的关系：

$$
\mathbf{d}_i^{(\ell)}[v]
\;\longleftrightarrow\;
\texttt{bitDom[level = }\ell\texttt{][var = i][word = }\lfloor v/32 \rfloor\texttt{]}
\text{ 中的第 } (v \bmod 32) \text{ 个 bit}
$$

删值操作：

$$
\mathbf{d}_i^{(\ell)}[v] \gets 0
$$

对应到实现就是：

```cpp
word &= ~(1u << (v % 32));  // 删除值 v
```

### 11.2 支持表的 bit 编码

考虑一个二元约束：

$$
c \in C, \quad \text{scope}(c) = (X_i, X_j)
$$

它的允许关系为：

$$
R_c \subseteq \mathcal{U} \times \mathcal{U}
$$

从 \(i \to j\) 方向，定义支持矩阵：

$$
S_c^{i \to j} \in \{0,1\}^{M \times M}, \quad
S_c^{i \to j}(a,b) =
\begin{cases}
1, & (a,b) \in R_c \\
0, & (a,b) \notin R_c
\end{cases}
$$

对每个 \(a \in \mathcal{U}\)，取出一行作为「支持集合」的 bit 向量：

$$
\mathbf{s}_{c,i,a} \in \{0,1\}^M, \quad
\mathbf{s}_{c,i,a}[b] = S_c^{i \to j}(a,b)
$$

即：

$$
\text{Supp}_{c,i}(a)
  = \{\, b \in \mathcal{U} \mid \mathbf{s}_{c,i,a}[b] = 1 \,\}
  \subseteq \mathcal{U}
$$

在实现中，对应关系是：

$$
\mathbf{s}_{c,i,a}
\;\longleftrightarrow\;
\texttt{bitSupData[cid][dir = i→j][val = a][word]}
$$

从 \(j \to i\) 的方向同理有 \(S_c^{j \to i}\) 与 \(\mathbf{s}_{c,j,b}\)，两组方向打包在 `uint2` 里。

### 11.3 单个约束上的 GAC 更新（bit-AND + 非零判断）

在层级 \(\ell\) 上，当前域 bit 向量为 \(\mathbf{d}_i^{(\ell)}, \mathbf{d}_j^{(\ell)}\)。

对约束 \(c=(X_i,X_j)\)，从 \(i \to j\) 方向，GAC 条件是：

> \(a\) 在 \(X_i\) 的域中可接受，当且仅当它在 \(X_j\) 当前域中存在至少一个支持。

集合形式：

$$
a \in D_i^{(\ell)} \text{ 在 } c \text{ 下有支持}
\iff
\exists b \in D_j^{(\ell)} \text{ 使得 } (a,b) \in R_c
$$

bit 编码下，把「存在支持」写成向量逻辑或：

$$
\exists b:\ \mathbf{s}_{c,i,a}[b] = 1 \land \mathbf{d}_j^{(\ell)}[b] = 1
\iff
\bigvee_{b=0}^{M-1} \big( \mathbf{s}_{c,i,a}[b] \land \mathbf{d}_j^{(\ell)}[b] \big) = 1
$$

定义中间向量：

$$
\mathbf{t}_{c,i,a}^{(\ell)}
  := \mathbf{s}_{c,i,a} \land \mathbf{d}_j^{(\ell)} \in \{0,1\}^M
$$

则：

$$
\text{has\_support}_c^{i \to j}(a)
  := \left( \bigvee_{b=0}^{M-1} \mathbf{t}_{c,i,a}^{(\ell)}[b] \right)
  = \left( \mathbf{s}_{c,i,a} \land \mathbf{d}_j^{(\ell)} \neq \mathbf{0} \right)
$$

删值规则可以写成：

$$
\boxed{
  \mathbf{d}_i^{(\ell)\,\text{new}}[a] =
    \mathbf{d}_i^{(\ell)}[a] \land
    \text{has\_support}_c^{i \to j}(a)
}
$$

也就是：

$$
\boxed{
  \mathbf{d}_i^{(\ell)\,\text{new}}[a] =
    \mathbf{d}_i^{(\ell)}[a] \land
    \left(
      \bigvee_{b=0}^{M-1}
        \big(
          S_c^{i \to j}(a,b) \land \mathbf{d}_j^{(\ell)}[b]
        \big)
    \right)
}
$$

在实现层面，\(\bigvee\) 是按 32-bit word 分块做的：

$$
\exists k \text{ 使得 }
\big(\text{word\_sup}_k(a) \land \text{word\_dom}_k(j)\big) \neq 0
$$

同理，从 \(j \to i\) 方向有：

$$
\boxed{
  \mathbf{d}_j^{(\ell)\,\text{new}}[b] =
    \mathbf{d}_j^{(\ell)}[b] \land
    \left(
      \bigvee_{a=0}^{M-1}
        \big(
          S_c^{j \to i}(b,a) \land \mathbf{d}_i^{(\ell)}[a]
        \big)
    \right)
}
$$

### 11.4 全局 GAC 固定点（忽略 frontier 细节）

记第 \(t\) 轮传播后，变量 \(X_i\) 的域 bit 向量为 \(\mathbf{d}_i^{(t)}\)。

对每个约束 \(c=(X_i,X_j)\)，定义当前轮从 \(i \to j\) 方向得到的「保留掩码」：

$$
\mathbf{g}_{i,c}^{(t)}[a] =
  \bigvee_{b=0}^{M-1} \big(
    S_c^{i \to j}(a,b) \land \mathbf{d}_j^{(t)}[b]
  \big)
$$

全局更新（抽象掉 frontier，只看数学上的「所有相关约束都 enforce 一遍」）是：

$$
\mathbf{d}_i^{(t+1)}[a]
  =
  \mathbf{d}_i^{(t)}[a]
  \land
  \bigwedge_{c \in N(i)} \mathbf{g}_{i,c}^{(t)}[a]
$$

其中 \(N(i)\) 是所有包含 \(X_i\) 的约束集合。

GAC 固定点条件是：存在某个 \(T\) 使得

$$
\forall i,\ \mathbf{d}_i^{(T+1)} = \mathbf{d}_i^{(T)}
$$

或者等价地：

$$
\forall c=(X_i,X_j),\ \forall a\ \text{若}\ \mathbf{d}_i^{(T)}[a]=1,\ 
\text{则}\ \exists b\ \mathbf{d}_j^{(T)}[b]=1 \land (a,b)\in R_c
$$

以及对称的 \(j \to i\) 条件。

### 11.5 frontier bitmap 与邻接传播的抽象

约束集合 \(C = \{c_0,\dots,c_{m-1}\}\)，frontier 用 bit 向量表示：

$$
\mathbf{f}^{(t)} \in \{0,1\}^{|C|}, \quad
\mathbf{f}^{(t)}[k] = 1 \iff c_k \text{ 在第 } t \text{ 轮需要处理}
$$

变量–约束邻接矩阵（由 `d_subscription` / `d_subscription_offset` 给出）：

$$
A \in \{0,1\}^{n \times |C|}, \quad
A[i,k] = 1 \iff X_i \in \text{scope}(c_k)
$$

第 \(t\) 轮域发生变化的变量标记为：

$$
\Delta_i^{(t)} =
\begin{cases}
1, & \mathbf{d}_i^{(t+1)} \neq \mathbf{d}_i^{(t)} \\
0, & \text{otherwise}
\end{cases}
$$

组合成向量 \(\boldsymbol{\Delta}^{(t)} \in \{0,1\}^n\)。下一轮 frontier 可抽象为：

$$
\mathbf{f}^{(t+1)}[k] =
  \bigvee_{i=1}^n \big( \Delta_i^{(t)} \land A[i,k] \big)
$$

也就是「所有域发生变化的变量，把它们邻接到的约束的 bit 置 1」——与 `PropagateVarToNextBitmap` 的位运算逻辑一致。

---

## 参考文档

- [GMODEL_OPTIMIZATION_MEMO.md](GMODEL_OPTIMIZATION_MEMO.md) - 优化备忘
- [CP_SCHEME_RECOMMENDATION.md](cp_schemes/CP_SCHEME_RECOMMENDATION.md) - 方案推荐
- [CLAUDE.md](../CLAUDE.md) - 项目开发指南
