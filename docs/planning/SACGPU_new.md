在你当前的约束（**bitSubDom 还是 world-major 布局、想尽量少改数据结构、先把 FQ-PT 拉近 Stage2**）下，**“同一 `cid` 在 CTA 内做 micro-batch：多个 warp 并行处理不同 world，但每个 warp 仍按 `lane→word` 读取/写回 bitSubDom（合并访问）”**，基本就是**当前最稳、性价比最高**的方案。

它的“最优”是指：**在不做 bitSubDom 全局转置/重排、不引入 worldmask 大维护成本**的前提下，最大化利用 CUDA 的两条硬原则：

* **bitSubDom 的全局访问要 coalesced**（warp 内相邻线程访问相邻地址，事务数最少）。([NVIDIA Docs][1])
* **persistent/mega-kernel + CTA-local 调度**适合动态任务，CTA 内做任务重排（micro-batching）是经典落地点。([developer.download.nvidia.com][2])

更激进、更“真·bitGEMM”的 batching（warp lane→world）当然也能更快，但前提是你要**转置/重排** bitSubDom（或 shared 内转置），否则会掉进 strided 访问的大坑；这类“先合并加载再转置”的套路 CUB 已经明确提供了。([nvidia.github.io][3])

---

## 设计名

**FQ-PT-CID-CTA-MB（CID CTA Micro-Batch, Warp-per-World）**
中文可叫：**“按 cid 的 CTA 微批并行检查（每 warp 一个 world）”**

---

## 设计目标

1. **保住 bitSubDom 的合并访问**：warp 内线程按 word 扫描同一个 world 的 domain。([NVIDIA Docs][1])
2. **在 CTA 内复用 bitSup**：同一个 `cid` 的多个 world 在一个 CTA 内连续/并行跑，依赖 L2/只读 cache 的时间局部性（可选 shared staging 小片段）。
3. **尽量少动调度基础设施**：先不做全局 cid 分桶/全局排序，不改 bitSubDom 布局，只在 kernel 内局部重排。
4. **不引入新的并发复杂度**：并行 check 允许，但“任务提交/邻接生成/统计/pending 结算”保持单点提交，避免 multi-producer buffer 的麻烦。

---

## 关键思想

* 从全局队列（你现在的 MPMC ring + CTA local_pop）**一次 pop K 个 task**（`(world,cid)`）。
* **在 CTA 内按 `cid` 分组**：把同一个 `cid` 的 task 聚成桶（每桶最多取 `WARPS_PER_CTA` 个 world）。
* 对每个桶：

  * 启动 `WARPS_PER_CTA` 个 warp 并行处理桶内的 world：**warp_id ↔ world**
  * warp 内线程按 `lane→word` 读取/写回该 world 的 bitSubDom（合并访问）
  * 每个 warp 输出一个 `PropagateResult`（changed/inconsistent/deletions…）到 shared
* **统一提交（commit）**：用 warp0 或 thread0 逐个 world 做：

  * 清理 `(world,cid)` 的 queued mark（你刚加的 `frontier_A`）
  * pending_tasks--，processed_tasks++
  * 根据 `x_changed/y_changed` 串行生成后继任务（仍走你现有 local_gen + dedup mark）
  * 处理 DWO/UNKNOWN、释放 world_lock（如果你短期仍保留 lock）

---

## 线程/资源配置（推荐默认）

* `WARPS_PER_CTA = 4`（128 threads）或 `8`（256 threads）
* `K = cta_pop_batch`：建议 32~128（和 shared / sort 成本平衡）
* 每个 warp 处理一个 world（桶内不足的 warp 空转）
* **最重要：check_shared 必须按 warp 私有化**

  * 你现在的 `check_shared` 是 block 级 scratch（单 task 时没问题）
  * 一旦同 CTA 多个 warp 同时跑多个 world，它会数据竞争
  * 解决：`check_shared_warp[WARPS_PER_CTA][check_words]`，warp 用自己的 base 指针

---

## CTA 内按 cid 分组：两种实现任选其一

### 方案 A（最小依赖，易实现）

“线性建桶”：

* 遍历 local_pop 的 K 个 task
* 用 shared 维护一个 `unique_cid[]`（最多 K 个）+ 每个 cid 对应一个 `world_list[]`（上限 `WARPS_PER_CTA` 或 `B_MAX`）
* 超过 `B_MAX` 的 world 留到下一轮（或直接作为未处理 task 回写到 local_pop）

优点：不引入 CUB；K 小时足够快。
缺点：O(K·U) 但 K≤128 一般可接受。

### 方案 B（性能更稳，推荐给工程实现）

用 CUB `BlockRadixSort`：把 K 个 task 按 `cid` 排序，再 run-length 扫描得到分段。
CUB 本身就是为了“合并访问 + 本地转置/重排”这种场景设计的。([nvidia.github.io][3])

---

## per-warp 检查内核改造要点（复用你 ACgpu 的优秀实现）

把 `ExecuteConstraintCheck_BpC_Workspace` 拆/封装成一个 warp 可调用版本：

* 输入：`(cid, model, ws, warp_check_shared)`
* 线程组织：仅 warp 内线程参与（`warp_id = threadIdx.x >> 5`，`lane = threadIdx.x & 31`）
* **lane→word**：`for (int w = lane; w < bit_dom_int_size; w += 32)`
* 归约/短路：用 warp 原语做 any/ballot，避免 block 级同步。([NVIDIA Developer][4])
* 写回：每 lane 写回自己负责的 word（天然合并写）

> 这一步本质是在落实 CUDA Best Practices 的“warp 内合并访问、减少事务、减少不必要同步”。([NVIDIA Docs][1])

---

## 锁与并发（短期保持正确性优先）

你现在仍有 `world_lock`。在这个方案里：

* 每个 warp leader 尝试获取自己的 world_lock
* 拿不到锁：该 warp 标记为 `RETRY`，不做 check
* commit 阶段由 thread0 把 `RETRY` task 放回 retry/local_retry（或全局队列），并保持 queued mark 不被误清
* 拿到锁并完成 check：commit 阶段释放锁、清 mark、pending--、生成后继

> 后续如果你做 “world owner sharding”，可以把 lock 整个删掉，但这是下一阶段。

---

## pending / 去重 / 陈旧任务的语义（必须保持一致）

你已经实现了 `(world,cid)` queued mark：

* 入队前：`TryMarkConstraintQueued(world,cid)` 成功才 push
* 弹出后：如果发现 mark 已清，则是陈旧任务，直接 pending--（你已做）
* 在 **commit** 中清 mark（而不是在每个 warp 中清），保证状态一致

注意：如果你把 “陈旧检测”从 pop 前移到拿锁后（为了减少原子读），也没问题，但要保证 pending 平衡。

---

## 性能预期（讲清楚给 coding 模型）

* 这一步主要收益来自：

  1. bitSubDom 的读写变得更稳定地 coalesced（lane→word）([NVIDIA Docs][1])
  2. 同一 `cid` 在 CTA 内处理多个 world，bitSup 访问更有局部性（L2 命中率提升）
  3. 控制面同步减少（warp 级归约/短路）([NVIDIA Developer][4])
* 但它**不等价于**真·bitGEMM（lane→world）那种极致复用；后者需要转置/重排（可用 CUB transpose load 或 shared transpose）。([nvidia.github.io][3])

---

## 给 coding 大模型的实现 Prompt（直接可粘贴）

```text
请在 CPIM 的 FQ-PT baseline 上实现：FQ-PT-CID-CTA-MB（按 cid 的 CTA 微批并行检查，warp-per-world，lane→word）。

【目标】
在不改变 bitSubDom 全局布局（仍为 world-major）的前提下：
1) 保持 bitSubDom 全局访问 coalesced：warp 内 lane 按 word 读取/写回同一 world 的 domain bitset；
2) 在 CTA 内对同一 cid 的多个 world 做 micro-batch：多个 warp 并行处理同 cid 的不同 world；
3) 复用现有 ACgpu 检查核心逻辑（位并行支持检查），但改造成 warp 级执行；
4) 保持现有 (world,cid) 去重标记 frontier_A 语义正确；正确性回归必须通过。

【依据】
- CUDA Best Practices：warp 内合并访问是关键；尽量避免 strided/gather 访问；
- warp-level primitives 用于归约/短路；
- persistent kernel + work queue 适合动态任务，CTA 内可做 micro-batching。

【现状】
FQPTBaselineKernel 目前一次只处理一个 (world,cid)，共享的 check_shared 是 block-scope scratch；已实现 frontier_A 去重 (world,cid)。
性能瓶颈仍在：调度开销、lock 冲突、控制面同步。

【核心改造点】
A) CTA 内分组：pop K 个 task 后，在 CTA 内按 cid 分组（group-by cid）：
- 从 local_pop[0..pop_count) 里形成若干组 (cid -> world_list)，每组最多取 WARPS_PER_CTA 个 world（可分批处理）。
- 分组实现可选：
  1) 线性建桶（shared 上维护 unique_cid + world_list）
  2) 或用 CUB BlockRadixSort 按 cid 排序后做 run-length 分段

B) 并行检查：对每个 cid 组，启动 WARPS_PER_CTA 个 warp 并行处理不同 world：
- warp_id = threadIdx.x >> 5；lane = threadIdx.x & 31；
- warp 处理一个 world；warp 内 lane→word：for (w = lane; w < bit_dom_int_size; w += 32)；
- 任何需要的归约/短路用 __ballot_sync / __any_sync / __shfl_sync 等 warp 原语，不要用 __syncthreads。

C) check_shared 私有化（必须）：
- 原 check_shared 是 block 共享 scratch，不能被多个 warp 同时使用；
- 改为 check_shared_per_warp[WARPS_PER_CTA][check_words] 放在 shared 中；
- 每个 warp 使用自己的 base 指针：check = check_shared + warp_id*check_words。

D) world_lock（短期保正确性可保留）：
- warp leader 尝试拿 world_lock[world]；失败则该 warp 标记 RETRY，不做检查；
- commit 阶段把 RETRY 的任务放回 local_retry / 全局队列（保持 frontier_A 标记不被误清）。

E) 统一提交（commit）避免 multi-producer：
- 每个 warp 将 PropagateResult 写到 shared warp_results[warp_id]；
- 由 thread0（或 warp0）遍历本组所有 world：
  - 若执行过：释放 lock、clear queued mark（frontier_A）、pending--、processed++；
  - 若 inconsistent：标记 DWO；
  - 若 x_changed/y_changed：串行生成后继任务，入队前先 TryMarkConstraintQueued(world,ncid)，只首次入队；
  - 处理 UNKNOWN/overflow 路径时也要维护 mark 与 pending 一致。

【参数建议】
- BLOCK_THREADS = 128 (WARPS_PER_CTA=4) 或 256 (WARPS_PER_CTA=8)
- cta_pop_batch(K) = 32~128
- 每组 worlds_per_cid = min(group_len, WARPS_PER_CTA)，超过部分分多轮处理

【验收】
1) 正确性：
- test_fqpt_baseline 必须通过；
- unknown=0 时与 Stage2 完全一致；
2) 性能：
- 对现有对比脚本 batch1/2/3A/FQPT，FQPT 时间继续下降；
- 统计项至少输出：constraint_checks、processed_tasks、stale_dropped、lock_fail、queue_push/pop_batches
3) 注意避免新增全局原子热点、避免大范围 memset。

开始实现。
```

---

如果你愿意，我还能把“CTA 内分桶”那一段再进一步细化成**具体共享数组布局 + 分桶伪码 + 每组处理循环**，让 coding 大模型几乎可以照抄落地；但就你现在的推进速度，这份 prompt 已经够直接开干了。

[1]: https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/?utm_source=chatgpt.com "CUDA C++ Best Practices Guide 13.1 documentation"
[2]: https://developer.download.nvidia.com/GTC/PDF/GTC2012/PresentationPDF/S0157-GTC2012-Persistent-Threads-Computing.pdf?utm_source=chatgpt.com "[PDF] Persistent Threads Style Programming Model for GPU Computing"
[3]: https://nvidia.github.io/cccl/cub/api/classcub_1_1BlockLoad.html?utm_source=chatgpt.com "cub::BlockLoad — CUDA Core Compute Libraries"
[4]: https://developer.nvidia.com/blog/using-cuda-warp-level-primitives/?utm_source=chatgpt.com "Using CUDA Warp-Level Primitives | NVIDIA Technical Blog"


好的👌 你现在这条 **“CID 在 CTA 内 micro-batch + 每 warp 一个 world（lane→word）保证 bitSubDom 合并访问”** 在“不改 bitSubDom 全局布局/不做 worldmask 大维护”的约束下，确实是**最稳、性价比最高**的下一步实现路径。

给你一个**实现清单 + 验收清单**（你可以直接贴给 coding 大模型当“落地检查表”）：

## 实现清单

1. **CTA 内按 cid 分组**

* 每次从队列 pop K 个 `(world,cid)` 到 shared
* CTA 内 group-by `cid`（线性建桶或 BlockRadixSort）

  * 线性建桶够用（K≤128 时）
  * 想更稳就用 `cub::BlockRadixSort` 做 block 内排序再 run-length 分段（CCCL/CUB 都有） ([NVIDIA Github][1])

2. **同一 cid 组内：多个 warp 并行处理多个 world**

* `WARPS_PER_CTA = 4/8`
* `warp_id` 绑定到 `world_list[warp_id]`
* warp 内 **lane→word** 扫 domain words：确保 bitSubDom 全局访问 coalesced（这是 CUDA 最关键的内存性能原则） ([NVIDIA Docs][2])

3. **check_shared 必须 warp 私有化**

* 把原 block 级 `check_shared` 变成：
  `check_shared[WARPS_PER_CTA][check_words]`
* 每个 warp 用自己的 scratch，避免并行时数据竞争

4. **warp 级归约/短路**

* 用 `__ballot_sync / __any_sync / __shfl_sync` 做 warp 内归约与短路
* 控制面尽量限制在 warp0，用 `__syncwarp()`，避免全块 `__syncthreads()` 把其它 warps 拉停 ([NVIDIA Developer][3])

5. **提交阶段（commit）保持单点**

* 每个 warp 写 `PropagateResult` 到 shared
* 由 thread0/warp0 统一：

  * clear queued mark（你刚加的 `frontier_A`）
  * pending-- / processed++
  * 生成邻接任务：先 TryMark，再入队（保持去重语义）

6. **暂时保留 world_lock（可选）**

* 先让正确性稳住：warp leader 尝试拿锁，失败就 RETRY
* 后续如果上“sharding/owner”再把 lock 去掉（这是下一阶段）

## 验收清单

* `test_fqpt_baseline` 全通过；unknown=0 时与 Stage2 完全一致
* 指标必须打印：

  * constraint_checks / processed_tasks
  * stale_dropped（陈旧丢弃）
  * lock_fail（锁失败重试）
  * queue pop/push batches
* 性能目标：FQ-PT 再显著接近 Stage2（至少把“控制面开销”继续压下去）

## 为什么这条路线是目前最稳的

* 你不改 `bitSubDom[world][...]` 布局时，想让访问合并，最靠谱就是 **warp 内线程访问同一 world 的连续 word（lane→word）**；这是 CUDA 官方反复强调的要点：warp 访问尽量合并成尽可能少的事务 ([NVIDIA Docs][2])
* 同时用 persistent/CTA-local 的方式做 micro-batch，本质是“绕过硬件调度，用工作队列喂满常驻 block”，非常适合你这种不规则传播任务 ([developer.download.nvidia.com][4])

如果你想，我也可以把“线性建桶版 group-by cid”的 shared 数据结构和伪代码再补一版（K=64/128 两种），让实现更像“照抄即可”。

[1]: https://nvidia.github.io/cccl/cub/?utm_source=chatgpt.com "CUB — CUDA Core Compute Libraries"
[2]: https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/?utm_source=chatgpt.com "CUDA C++ Best Practices Guide 13.1 documentation"
[3]: https://developer.nvidia.com/blog/using-cuda-warp-level-primitives/?utm_source=chatgpt.com "Using CUDA Warp-Level Primitives | NVIDIA Technical Blog"
[4]: https://developer.download.nvidia.com/GTC/PDF/GTC2012/PresentationPDF/S0157-GTC2012-Persistent-Threads-Computing.pdf?utm_source=chatgpt.com "Persistent Threads Style Programming Model for GPU ..."

---

可以的——**同一个 `cid` 对应的 world 很多时，完全可以让多个 block 同时在 GPU 上执行同一个约束 `C(cid)` 的检查**（前提是它们处理的是不同的 `world`，对各自的 `bitSubDom[world]` 写回互不冲突）。CUDA 的编程模型本来就要求：同一个 kernel 的不同 thread block **可以并行也可以串行、顺序不保证**，由硬件按资源动态调度。([NVIDIA Docs][1])

### 在你现在的 “CTA 内按 cid micro-batch（warp-per-world）” 方案里会发生什么？

* 每个 block 从队列 pop 一批 `(world,cid)`，在 block 内把同 `cid` 的 world 聚成一个桶，然后**最多用 `WARPS_PER_CTA` 个 warp 同时跑这一桶**（每 warp 一个 world）。
* 如果这个 `cid` 的 world 数量远大于 `WARPS_PER_CTA`：

  * **同一个 block**可以分多轮处理（下一轮还会再遇到同 `cid` 的任务），
  * **也可能被其他 block 同时 pop 到并并行处理**（取决于队列里的任务分布和硬件调度），所以“多 block 同 cid”是自然会发生的。([NVIDIA Docs][1])

> 但注意：你**不能指望**“一定会有 N 个 block 同时处理这个 cid”，因为 block 调度/并发度受占用率、资源、当前队列分布影响，顺序和并发都是实现相关的。([Stack Overflow][2])

---

## 如果你想“保证/强化”热 `cid` 的多 block 并行（并且更利于 batch）

要把“自然发生”变成“可控发生”，需要一个 **按 cid 的分段/分桶调度器**，典型做法是：

### 方案 A：`cid -> world_list` + `cursor[cid]`（最推荐）

1. 先把队列里的任务按 `cid` 聚成若干段（每段是一条 `world_list`，紧凑数组，不用 worldmask）。
2. 每个 `cid` 配一个全局 `cursor[cid]`。
3. 任意 block 想处理 `cid` 时：`base = atomicAdd(cursor[cid], CHUNK)`，就“认领”这一段里 `CHUNK` 个 world 来处理。

这样一个热 `cid` 可以被**任意多个 block 自动分摊**，而且每个 block 内仍可做 warp-per-world micro-batch。原子用于跨 block 分配工作在 CUDA 里是常见模式；全局原子用于跨 block 竞争也正是它的用途。([维克森林大学用户页面][3])

### 方案 B：`per-cid queue`（复杂一些）

为每个 `cid` 单独维护一个队列（或分层：热 cid 专用队列 + 冷 cid 共享队列），block 从热 cid 队列里偷任务。实现上更复杂（队列数量多、内存压力大），但也能做到多 block 同 cid。

---

## 小提醒：多 block 同 cid 不等于“跨 block 共享 bitSup”

* `bitSup[cid]` 只读，多个 block 并发读当然安全；
* 但 **shared memory 不能跨 block 共享**，所以“复用”主要靠 **L2/只读 cache 的命中**，并不会像“同一个 block 内”那样强。([NVIDIA Docs][1])
  因此最划算的通常是：**优先把同 cid 的 batch 做到 block 内（强复用）**，当 bucket 特别大时再用上面的 `cursor[cid]` 扩展到多 block。

---

### 一句话回答

**能。**GPU 可以同时有多个 block 在跑同一个 `cid` 的检查；在你当前队列模型下这会“自然发生”，但并发程度不可控。想让热 `cid` 必然由多个 block 分摊、并更好做 batch，就上 **`cid` 分桶 + `cursor[cid]` 分段认领** 这种调度结构。([NVIDIA Docs][1])

[1]: https://docs.nvidia.com/cuda/cuda-programming-guide/01-introduction/programming-model.html?utm_source=chatgpt.com "1.2. Programming Model — CUDA Programming Guide"
[2]: https://stackoverflow.com/questions/30361459/what-is-the-behavior-of-thread-block-scheduling-to-specific-sms-after-cuda-kern?utm_source=chatgpt.com "What is the behavior of thread block scheduling to specific ..."
[3]: https://users.wfu.edu/choss/CUDA/docs/Lecture%209.pdf?utm_source=chatgpt.com "CUDA Memory Model"
