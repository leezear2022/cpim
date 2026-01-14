好，那我按我们刚才说的“合并版路线”，给你一个**可以直接照着实现的开发计划**，里边会包含关键函数的**C++/CUDA skeleton**，但刻意留出一些 `TODO`，方便你按自己的 code style/调试习惯补完。

下面所有东西都是围绕你现有的：

* `GModel` / `GModelSolver`
* `bitDom / bitSupData / constraint_scopes / d_subscription / d_subscription_offset`
* 多层级 `bitDom[level * bit_doms_int_size + ...]` 和 `current_level_`

来设计的。

---

## 总体路线图（你可以当成 3 个阶段）

**阶段 0（准备）：**

* 在 `GModel` 里加上 GPU 传播控制结构（`GACControl` + bitmap buffer）。
* 提供 `InitializeGPUResources()` / `FreeGPUResources()`。

**阶段 1（非 CG 版本，先跑通 / 便于调试）：**

* 实现：

  * `__device__ ExecuteConstraintCheck_BpC(...)`：从 `CsCheckMainKernel` 抽出来；
  * `__global__ BitmapGACKernel(...)`：一轮吃完 `frontier_cur`；
  * `GModel::EnforceGAC(bool verbose, int assigned_var)`：host while + 多次 launch kernel 完成迭代。

**阶段 2（CG 版本，可选优化）：**

* 把多个 kernel 轮次内联到一个 **cooperative persistent kernel** `PersistentGACKernel` 里。
* EnforceGAC 只需一次 `cudaLaunchCooperativeKernel`。

下面的 skeleton 以**阶段 1 为主**，因为这是你最容易先贴进去跑的版本；阶段 2 只给出你怎么从它演进即可。

---

## 一、在 GModel 里增加传播控制结构

### 1. `GACControl` + `GModelData`（头文件中）

在 `GModel.cuh` 里加：

```cpp
struct GACControl {
  int inconsistent_flag;   // 0 or 1
  int scanner_index;       // word-based bitmap扫描游标
  long long deletions;     // 累计删值数
  int iterations;          // 传播轮数（frontier轮数）
};

struct GModelData {
  int num_vars;
  int num_constraints;
  int max_dom_size;
  int bit_dom_int_size;
  int bit_doms_int_size;
  int bitsup_per_constraint;

  u32*       bitDom;
  int*       d_cur_dom_size;
  const uint2* bitSupData;
  const int2*  constraint_scopes;
  const uint3* d_subscription;
  const int*   d_subscription_offset;
};
```

在 `class GModel` 中加成员：

```cpp
class GModel {
public:
  // ... 你已有的字段 ...

  // GAC GPU 控制资源
  GACControl* d_gac_control = nullptr;
  u32* d_queue_bitmap_A = nullptr;  // current frontier
  u32* d_queue_bitmap_B = nullptr;  // next frontier
  int  bitmap_size_words = 0;

  // 初始化/释放
  void InitializeGPUResources();
  void FreeGPUResources();
  GModelData GetGModelDataView() const;

  // 新版传播接口
  GacStats EnforceGAC(bool verbose = true, int assigned_var = -1);

  // ...
};
```

### 2. `InitializeGPUResources` / `FreeGPUResources`（GModel.cu）

```cpp
void GModel::InitializeGPUResources() {
  if (d_gac_control) return;  // 已经初始化

  bitmap_size_words = (num_constraints + 31) / 32;

  cudaMalloc(&d_gac_control, sizeof(GACControl));
  cudaMalloc(&d_queue_bitmap_A, bitmap_size_words * sizeof(u32));
  cudaMalloc(&d_queue_bitmap_B, bitmap_size_words * sizeof(u32));
  // TODO: 加上 error check（可用一个小宏包装）
}

void GModel::FreeGPUResources() {
  if (d_gac_control) {
    cudaFree(d_gac_control);
    d_gac_control = nullptr;
  }
  if (d_queue_bitmap_A) {
    cudaFree(d_queue_bitmap_A);
    d_queue_bitmap_A = nullptr;
  }
  if (d_queue_bitmap_B) {
    cudaFree(d_queue_bitmap_B);
    d_queue_bitmap_B = nullptr;
  }
}
```

记得在 `GModel::~GModel()` 里调用 `FreeGPUResources()`。

### 3. `GetGModelDataView`

```cpp
GModelData GModel::GetGModelDataView() const {
  GModelData md;
  md.num_vars            = num_vars;
  md.num_constraints     = num_constraints;
  md.max_dom_size        = max_dom_size;
  md.bit_dom_int_size    = bit_dom_int_size;
  md.bit_doms_int_size   = bit_doms_int_size;
  md.bitsup_per_constraint = bitsup_per_constraint;

  md.bitDom              = bitDom;
  md.d_cur_dom_size      = d_cur_dom_size;
  md.bitSupData          = bitSupData;
  md.constraint_scopes   = constraint_scopes;
  md.d_subscription      = d_subscription;
  md.d_subscription_offset = d_subscription_offset;
  return md;
}
```

---

## 二、设备端工具函数 & BpC 核心

### 1. 变量 → 邻接约束的传播（Producer）

```cuda
__device__ __forceinline__
void PropagateVarToNextBitmap(
    int var,
    const GModelData& model,
    u32* next_bitmap) {

  const int start = model.d_subscription_offset[var];
  const int end   = model.d_subscription_offset[var + 1];

  for (int i = start; i < end; ++i) {
    int cid = model.d_subscription[i].z; // 你现在uint3里cid在z
    int w = cid / 32;
    int b = cid % 32;
    atomicOr(&next_bitmap[w], 1u << b);
  }
}
```

### 2. 从 bitmap 取任务（word 级调度）

```cuda
__device__ __forceinline__
int FetchNextCidFromBitmap(
    u32* frontier_cur,
    int bitmap_size_words,
    int* scanner_index,
    int& local_word,
    int& local_offset) {

  while (true) {
    if (local_word != 0) {
      int bit = __ffs(local_word) - 1;
      local_word &= ~(1u << bit);
      return local_offset + bit;
    }
    int w = atomicAdd(scanner_index, 1);
    if (w >= bitmap_size_words) {
      return -1; // 本轮没有任务了
    }
    u32 word = atomicExch(&frontier_cur[w], 0u);
    if (word == 0u) {
      continue; // 这个word没有任务，继续抢下一word
    }
    local_word   = word;
    local_offset = w * 32;
  }
}
```

### 3. BpC 核心 skeleton（从 `CsCheckMainKernel` 抽）

这里只给 skeleton，你可以直接把你现有 `CsCheckMainKernel` 中“单约束传播”的主体搬进来：

```cuda
struct PropagateResult {
  bool x_changed;
  bool y_changed;
  bool inconsistent;
  int  deletions;
};

__device__
PropagateResult ExecuteConstraintCheck_BpC(
    int cid,
    const GModelData& model,
    int current_level,
    u32* shared_mem) {

  PropagateResult r{};
  r.x_changed = r.y_changed = r.inconsistent = false;
  r.deletions = 0;

  const int2 scope = model.constraint_scopes[cid];
  const int x = scope.x;
  const int y = scope.y;
  if (x < 0 || y < 0) {
    return r;
  }

  const int level_offset = current_level * model.bit_doms_int_size;
  u32* dom_x = model.bitDom + level_offset + x * model.bit_dom_int_size;
  u32* dom_y = model.bitDom + level_offset + y * model.bit_dom_int_size;

  u32* s_dom_x = shared_mem;
  u32* s_dom_y = shared_mem + model.bit_dom_int_size;

  // 1) load 到 shared
  for (int w = threadIdx.x; w < model.bit_dom_int_size; w += blockDim.x) {
    s_dom_x[w] = dom_x[w];
    s_dom_y[w] = dom_y[w];
  }
  __syncthreads();

  // 2) 每个线程遍历若干 value
  for (int val = threadIdx.x; val < model.max_dom_size; val += blockDim.x) {
    const int word = val / 32;
    const int bit  = val % 32;
    if (word >= model.bit_dom_int_size) break;
    const u32 mask = 1u << bit;

    const bool active_x = (s_dom_x[word] & mask) != 0u;
    const bool active_y = (s_dom_y[word] & mask) != 0u;

    bool keep_x = true, keep_y = true;

    if (active_x) {
      const int sup_idx_base_x =
          cid * model.bitsup_per_constraint +
          (0 * model.max_dom_size + val) * model.bit_dom_int_size;
      bool has_sup = false;
      #pragma unroll
      for (int w = 0; w < model.bit_dom_int_size; ++w) {
        has_sup |= (model.bitSupData[sup_idx_base_x + w].x & s_dom_y[w]) != 0;
      }
      keep_x = has_sup;
    }

    if (active_y) {
      const int sup_idx_base_y =
          cid * model.bitsup_per_constraint +
          (1 * model.max_dom_size + val) * model.bit_dom_int_size;
      bool has_sup = false;
      #pragma unroll
      for (int w = 0; w < model.bit_dom_int_size; ++w) {
        has_sup |= (model.bitSupData[sup_idx_base_y + w].y & s_dom_x[w]) != 0;
      }
      keep_y = has_sup;
    }

    if (active_x && !keep_x) {
      atomicAnd(&s_dom_x[word], ~mask);
    }
    if (active_y && !keep_y) {
      atomicAnd(&s_dom_y[word], ~mask);
    }
  }
  __syncthreads();

  // 3) 写回 global + 统计删值 + DWO
  if (threadIdx.x == 0) {
    int new_size_x = 0, new_size_y = 0;

    for (int w = 0; w < model.bit_dom_int_size; ++w) {
      const u32 old_x = dom_x[w];
      const u32 new_x = s_dom_x[w];
      const u32 removed_x = old_x & ~new_x;
      if (removed_x) {
        atomicAnd(&dom_x[w], new_x);
        r.x_changed = true;
        r.deletions += __popc(removed_x);
      }

      const u32 old_y = dom_y[w];
      const u32 new_y = s_dom_y[w];
      const u32 removed_y = old_y & ~new_y;
      if (removed_y) {
        atomicAnd(&dom_y[w], new_y);
        r.y_changed = true;
        r.deletions += __popc(removed_y);
      }
    }

    // 重新计算域大小（可优化）
    const int base_x = level_offset + x * model.bit_dom_int_size;
    const int base_y = level_offset + y * model.bit_dom_int_size;
    for (int w = 0; w < model.bit_dom_int_size; ++w) {
      new_size_x += __popc(model.bitDom[base_x + w]);
      new_size_y += __popc(model.bitDom[base_y + w]);
    }
    model.d_cur_dom_size[current_level * model.num_vars + x] = new_size_x;
    model.d_cur_dom_size[current_level * model.num_vars + y] = new_size_y;

    if (new_size_x == 0 || new_size_y == 0) {
      r.inconsistent = true;
    }
  }

  // TODO: 可以把 r 的字段写到 shared，再 __syncthreads()，实现 block 范围广播
  __syncthreads();

  // 简化起见：只让 threadIdx.x == 0 返回有意义的数据
  return r;
}
```

---

## 三、非 CG 版 Bitmap GAC kernel（单轮）

一轮“吃完 current frontier”：

```cuda
__global__
void BitmapGACKernel(
    GModelData model,
    GACControl* control,
    u32* frontier_cur,
    u32* frontier_next,
    int bitmap_size_words,
    int current_level) {

  extern __shared__ u32 shmem[];
  int local_word   = 0;
  int local_offset = 0;

  while (true) {
    __shared__ int cid_shared;
    if (threadIdx.x == 0) {
      int cid = FetchNextCidFromBitmap(
          frontier_cur,
          bitmap_size_words,
          &control->scanner_index,
          local_word,
          local_offset);
      cid_shared = cid;
    }
    __syncthreads();

    int cid = cid_shared;
    if (cid < 0) {
      break; // 当前轮 frontier 用完
    }

    // 传播
    PropagateResult r =
        ExecuteConstraintCheck_BpC(cid, model, current_level, shmem);

    if (threadIdx.x == 0) {
      if (r.deletions > 0) {
        atomicAdd(&control->deletions, (long long)r.deletions);
        const int2 scope = model.constraint_scopes[cid];
        if (r.x_changed) {
          PropagateVarToNextBitmap(scope.x, model, frontier_next);
        }
        if (r.y_changed) {
          PropagateVarToNextBitmap(scope.y, model, frontier_next);
        }
      }
      if (r.inconsistent) {
        atomicExch(&control->inconsistent_flag, 1);
      }
    }
    __syncthreads();

    if (control->inconsistent_flag) {
      break; // 快速逃出
    }
  }
}
```

---

## 四、`EnforceGAC(bool verbose, int assigned_var)`：host side 迭代控制（非 CG 版）

这是你最关心的部分之一，完整 skeleton 如下：

```cpp
GacStats GModel::EnforceGAC(bool verbose, int assigned_var) {
  GacStats stats;
  InitializeGPUResources();

  // 1. 初始化GAC控制块
  GACControl h_ctrl{};
  h_ctrl.inconsistent_flag = 0;
  h_ctrl.scanner_index     = 0;
  h_ctrl.deletions         = 0;
  h_ctrl.iterations        = 0;
  cudaMemcpy(d_gac_control, &h_ctrl,
             sizeof(GACControl), cudaMemcpyHostToDevice);

  // 2. 清空 next frontier
  cudaMemset(d_queue_bitmap_B, 0, bitmap_size_words * sizeof(u32));

  // 3. 初始化 current frontier
  if (assigned_var < 0) {
    // 初始 GAC: 所有约束激活
    std::vector<u32> h_bitmap(bitmap_size_words, 0u);
    for (int cid = 0; cid < num_constraints; ++cid) {
      if (constraint_scopes[cid].x < 0) continue;
      int w = cid / 32;
      int b = cid % 32;
      h_bitmap[w] |= (1u << b);
    }
    cudaMemcpy(d_queue_bitmap_A, h_bitmap.data(),
               bitmap_size_words * sizeof(u32), cudaMemcpyHostToDevice);
  } else {
    // 增量 GAC: 只激活 assigned_var 邻接约束
    std::vector<u32> h_bitmap(bitmap_size_words, 0u);
    const int start = d_subscription_offset[assigned_var];
    const int end   = d_subscription_offset[assigned_var + 1];
    for (int i = start; i < end; ++i) {
      int cid = d_subscription[i].z;
      int w = cid / 32;
      int b = cid % 32;
      h_bitmap[w] |= (1u << b);
    }
    cudaMemcpy(d_queue_bitmap_A, h_bitmap.data(),
               bitmap_size_words * sizeof(u32), cudaMemcpyHostToDevice);
  }

  GModelData md = GetGModelDataView();

  bool done = false;
  while (!done) {
    // 每轮迭代
    ++h_ctrl.iterations;
    h_ctrl.scanner_index = 0;
    cudaMemcpy(&d_gac_control->scanner_index,
               &h_ctrl.scanner_index,
               sizeof(int), cudaMemcpyHostToDevice);

    int blocks  = std::min(bitmap_size_words, 128);      // 可调
    int threads = std::min(max_dom_size, 256);           // BpC 并行度
    size_t shmem_bytes = 2 * bit_dom_int_size * sizeof(u32);

    BitmapGACKernel<<<blocks, threads, shmem_bytes>>>(
        md,
        d_gac_control,
        d_queue_bitmap_A,
        d_queue_bitmap_B,
        bitmap_size_words,
        current_level_);
    cudaDeviceSynchronize(); // 简单起见

    // 4. 检查 inconsistent / 下一轮是否为空
    cudaMemcpy(&h_ctrl, d_gac_control,
               sizeof(GACControl), cudaMemcpyDeviceToHost);

    if (h_ctrl.inconsistent_flag) {
      stats.inconsistent = true;
      done = true;
    } else {
      // 检查 d_queue_bitmap_B 是否全 0
      std::vector<u32> h_next(bitmap_size_words);
      cudaMemcpy(h_next.data(), d_queue_bitmap_B,
                 bitmap_size_words * sizeof(u32), cudaMemcpyDeviceToHost);

      bool non_empty = false;
      for (int i = 0; i < bitmap_size_words; ++i) {
        if (h_next[i] != 0u) { non_empty = true; break; }
      }
      if (!non_empty) {
        done = true; // 达到不动点
      } else {
        // swap A/B, 清空新的B
        std::swap(d_queue_bitmap_A, d_queue_bitmap_B);
        cudaMemset(d_queue_bitmap_B, 0, bitmap_size_words * sizeof(u32));
      }
    }
  }

  stats.deletions   = (int)h_ctrl.deletions;
  stats.iterations  = h_ctrl.iterations;
  // 其他字段（time等）由外层GModelSolver统计

  if (verbose) {
    std::cout << "[GAC-Bitmap] iterations=" << stats.iterations
              << " deletions=" << stats.deletions
              << " inconsistent=" << (stats.inconsistent ? "true" : "false")
              << std::endl;
  }
  return stats;
}
```

> 注意：
>
> * 为简单起见，这里每轮都把 `d_queue_bitmap_B` copy 回 host 检查是否为空，后续可以在 GPU 端加一个 `next_nonempty_flag`，减少拷贝；
> * 这已经比旧版 CPU queue + kernel 的模式先进很多，而且结构清晰，便于你逐步优化。

---

## 五、GModelSolver 集成：增量 GAC

`GModelSolver::Search` 里的调用：

```cpp
// 原来：
GacStats gac_stats = model_->EnforceGAC(false);

// 改成：
GacStats gac_stats = model_->EnforceGAC(false, var);
```

其他逻辑（level 管理、回溯、统计）不用动，你已经写得很清晰了。

---

## 六、之后要做的（Phase 2）一行提示

等你把上面这套跑通、验证正确性后：

1. 把 host 侧 `while (!done)` 推进到 GPU，把 `BitmapGACKernel` 进化为 `PersistentGACKernel` + cooperative launch；
2. 把 “检查 `d_queue_bitmap_B` 是否为空” 也移到 device 侧（类似我之前给你写的 `ParallelIterateAndSwap` + `cg::grid_group`）；
3. 再考虑 bitmap 稀疏情况下的 hybrid worklist（stream compaction + array queue）。

---

如果你愿意，下一步可以是：
你先贴一版你按上述 skeleton 改好的 `EnforceGAC` / kernel，我帮你逐行 check 一次，重点看边界条件、index 计算和 shared mem 用法有没有坑。
