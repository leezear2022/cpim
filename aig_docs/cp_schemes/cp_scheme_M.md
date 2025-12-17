# GPU并行传播方案（M）
---

## 1. 总体定位：GPU-first 的事件驱动 AC/SAC 引擎

**核心一句话：**

> 用 GPU 上的持久化 kernel + 设备端工作队列，跑一个“事件驱动的 AC/SAC 传播”，
> 事件就是“某个约束要重新检查”，数据面用你现有的 bitDom/bitSup 位矩阵核。

也就是把传统 CP 求解器里的 **“基于事件队列的传播引擎”** 搬到 GPU 上，形成一个 **多生产者、多消费者并发的 GAC/SAC 引擎**。

---

## 2. 抽象层：事件模型与状态

### 2.1 约束网络与位矩阵

* 变量：`x ∈ {0…num_vars-1}`，每个变量有有限论域；
* 约束：当前专注于**二元表约束** `C_c(x, y)`；
* 表示方式：

  * `bitDom[level][var]`：变量在某个 search level 的域，用 bitset 表示；
  * `bitSup[cid][var_side][val]`：支持矩阵，用 bitset 表示“对面变量的哪些值与 (var,val) 相容”。

也就是你现在 GModel 里的 `bitDom` + `bitSupData` 结构。

### 2.2 事件类型

定义两个逻辑事件：

1. **VarEvent(var)**：变量 `var` 的域发生了变化；
2. **ConEvent(cid)**：约束 `cid` 需要重新传播一次。

在实际引擎里只执行 `ConEvent`，`VarEvent` 是逻辑上的——通过邻接表扩展为若干 `ConEvent`：

* 变量→约束邻接：`var_to_constraints[var] = {cid_1, cid_2, ...}`；
* 域变化时：对每个 `cid ∈ var_to_constraints[var]` 生成 `ConEvent(cid)`。

### 2.3 GPU 上的全局状态

* **域状态**：`bitDom[level]`、`d_cur_dom_size[level]`（都在统一内存/设备端）；
* **工作队列**：存放 `ConEvent` 的队列；
* **订阅表**（邻接关系）：`var → constraints`；
* **全局标志**：

  * `inconsistent`：是否已发现空域；
  * 统计量：删值数、迭代轮数等。

---

## 3. 物理层：持久化 kernel + 设备端 MPMC 工作队列

### 3.1 工作项：ConEvent

* 队列元素：`ConEvent = {cid, level}`（必要时可带 instance_id 做多实例）。
* 语义：**对约束 cid 在当前 level 上执行一次传播**。

### 3.2 队列设计（思想层面）

采用一个**多生产者、多消费者（MPMC）的环形队列**：

* 全局数组 `queue[Q]`，配两个原子索引：

  * `head`：消费者从这里取任务；
  * `tail`：生产者在这里写入新任务；
* 每次入队/出队通过 `atomicAdd`/`atomicCAS` 更新 `head/tail`；
* 为了防止重复入队，还配一个 `in_queue[cid]` 标记位：

  * 入队前先 `if (!in_queue[cid].exchange(true)) push(cid)`；
  * 约束执行完可按需清掉 `in_queue[cid]` 或交由外层清理。

这是一个 GPU 版的 lock-free MPMC 队列思想。

> 后来我们发现“显式队列 + 原子冲突”的代价比较大，所以 G 方案才改用 bitmap workset。但在 M 方案里，队列是正面主角。

### 3.3 持久化 kernel 的执行模型

**Persistent kernel：** 启动若干个 blocks/warps，长期驻留在 GPU 上，反复从队列中取 `ConEvent` 执行。

大致伪流程：

```cpp
__global__ void PropagationWorkerKernel(GlobalState state) {
    while (true) {
        // 1. 从队列中取一个 ConEvent
        ConEvent ev;
        bool ok = dequeue(ev);   // 使用全局 head/tail + 原子操作
        if (!ok) {
            // 没有事件：可以自旋等待一段时间，或参与终止检测
            if (global_termination_check()) break;
            continue;
        }

        int cid   = ev.cid;
        int level = ev.level;

        // 2. 对约束 cid 做一次传播
        PropagateResult res = PropagateConstraint(cid, level, ...);

        // 3. 处理结果
        if (res.inconsistent) {
            atomicExch(&state.inconsistent, 1);
        }

        if (res.x_changed || res.y_changed) {
            // 用 var_to_constraints 展开 VarEvent
            for (int var in {scope[cid].x, scope[cid].y}) {
                for (int cid2 : var_to_constraints[var]) {
                    // 尝试入队新的 ConEvent(cid2)
                    if (!in_queue[cid2].exchange(true)) {
                        enqueue({cid2, level});
                    }
                }
            }
        }
    }
}
```

特点：

* 各个 block/warp 是**多消费者**，对同一队列抢任务；
* 域变化产生新的 `ConEvent`，即**多生产者**；
* AC/SAC 的传播是单调 shrinking，因此乱序执行不会破坏正确性。

---

## 4. 单约束传播核：bit-matrix AC/SAC

队列里的每个 `ConEvent(cid)` 最终会调用一个类似你现有 `CsCheckMainKernel` 的传播函数，只是从“独立 kernel”变成“持久 kernel 内部的设备函数”。

### 4.1 per-constraint 粒度

* 约束作用域： `(x, y) = scope[cid]`；
* 数据：

  * `bitDom[level][x]` 和 `bitDom[level][y]`；
  * `bitSup[cid]` 的两方向支持矩阵。

### 4.2 位矩阵 AC 检查

对每个值 `a` 以及 `b` 做：

* 若 `a ∈ Dom(x)`，检查是否存在 `b ∈ Dom(y)` 使 `(a,b)` 在 `bitSup` 中相容，若不存在则删 `a`；
* 对 `b ∈ Dom(y)` 同理。

利用位矩阵：

```text
for each value a:
  sup_x[a] = bitSup[cid][x→y][a]      // bitset over y's values
  has_support_x[a] = (sup_x[a] & Dom(y)) != 0
```

用 warp / block 内并行，对 `val` 做 “bitSup 行”与对面域的按位与+归约。

### 4.3 更新域并记录变化

* 在 shared memory 中先更新局部 `dom_x`, `dom_y` 的 bitset；
* 然后由一个线程把差异写回全局 `bitDom[level]`；
* 重算或增量更新 `d_cur_dom_size[level][x/y]`；
* 若 `Dom(x)` 或 `Dom(y)` 变空，置 `res.inconsistent = true`；
* 若有任意 bit 被删，置 `res.x_changed / res.y_changed = true`，并统计删值数。

> 核心思想：单约束传播是高度 SIMD 化的 bit-matrix 计算；队列只负责“调度哪条约束何时执行”。

---

## 5. 与搜索树和 GModelSolver 的衔接

### 5.1 初始传播（Level 0）

* 构建 GModel 后，初始化队列：

  * 把所有有效二元约束 `cid` 全部入队（或对应的 `in_queue[cid]` = true + `ConEvent`）；
* 启动 `PropagationWorkerKernel`（或在 solver 启动时常驻）；
* 等传播队列稳定：

  * 若 `state.inconsistent = true` → 问题无解；
  * 否则得到 GAC 状态的初始域。

### 5.2 搜索时的增量传播

在 `GModelSolver::Search(level, ...)` 的每步 assignment：

1. 调 `GModel::CreateNewLevel()`，复制上一层域到新 level；
2. 调 `AssignValue(var, value, level)` 设置单值域；
3. 生成一批初始事件：

   * 逻辑上是 `VarEvent(var)`，实现上直接把 `var` 的所有邻接约束 `cid` 作为新的 `ConEvent` 入队；
4. 通知 GPU 引擎在该 level 上做一次增量传播，直到队列空或发现空域；
5. 检查结果：

   * 若 inconsistency → 搜索回溯；
   * 若域都非空 → 继续选择下一个变量/值。

这样，搜索树过程中的“传播”完全由 GPU 队列引擎承担，CPU 只负责：

* 维护决策层级；
* 调用 `AssignValue` / `BackToLevel`；
* 拿到 `GacStats`（删值数、迭代轮数等）。

---

## 6. 想法层的特点与优劣

**特点：**

1. **GPU-first 的事件驱动模型**

   * 把 AC/SAC 视作大量“约束级任务”的事件流，用 GPU 持久 kernel + MPMC 队列驱动；
   * 多生产者（域变化处处发事件）、多消费者（blocks/warps 抢队列）天然适配你的“高动态并发”目标。

2. **数据面 = bit-matrix 传播核**

   * 每个事件内部的传播核就是你已有的 bitDom/bitSup 位矩阵运算；
   * 思想上是把 AC/SAC 统一为 batched vec×mat / bit-matrix 乘运算。

3. **CPU 只在“控制平面”上行动**

   * CPU 不再维护 GAC 队列，只负责：assignment → 通知 GPU → 读出统计/结果；
   * 有利于将来扩展到多实例、多搜索树节点并发。

**主要劣势（相对于后来 G 方案）：**

* 显式 MPMC 队列本身会带来 **较重的原子争用开销**（head/tail + in_queue 标记），而单次约束传播又很轻量，容易出现“控制比计算贵”的情况；
* 去重逻辑依赖 `in_queue` 位图 + 队列，对 GPU 来说比“一个 bitmap frontier”更复杂；
* 不够贴合 GPU 上常见的“frontier bitmap + 层次扫描”模式，所以我们后来才倾向于用 **位图工作集（G 方案）** 做主干，把这种队列式实现当成稀疏/优先级场景的备选。

---

这就是**我最初给你提的那套传播引擎（M 方案）**的完整思想版总结：

* 关键词可以概括为：
  **“GPU-first event-driven propagation, persistent kernel + device MPMC queue, constraint-level tasks, bit-matrix AC/SAC 内核。”**

你可以把这份总结连同 C 方案、G 方案扔给别的模型，看它们怎么对比三者的思路优劣。需要的话我也可以再帮你做一个“三者对比表（只比思想维度）”的精简版。
