# Batched AC 与 SAC：从 bitGEMV 到 bitGEMM（说明稿）

你提出的类比方向（AC≈bitGEMV、SAC≈bitGEMM、GPU擅长GEMM）在**概念/算子层面是成立的**；但要严谨，需要补两个限定：

1) 这里的 GEMV/GEMM 是 **布尔半环（OR-AND）的 bitGEMV/bitGEMM**，不是数值 GEMM；性能主要受内存带宽与数据复用影响。  
2) “batched AC = SAC”严格对应的是**一次 SAC 检查（SAC-checking pass）**：给定当前域状态，对一批单例世界各自跑 AC 到不动点并判一致/不一致。要达到 **SAC 闭包（SAC-closure / fixed point）**，还需要外层重复该 pass 直到不再删值。

下文把命题拆成两层来写：

- 理论层：说明“batched AC 等价于并行做一批 singleton-check（SAC-checking pass）”。  
- 算子层：在你当前 bit 编码下，单世界 AC 的支持判定是 boolean bitGEMV，批量世界则是 boolean bitGEMM。

---

## 0. 适用范围（不写清楚会被抓漏洞）

本文的推导与类比，默认满足以下前提：

1) **约束类型**：只讨论二元 **Extension / supports** 表约束（允许对偶值对集合）。  
   这样才能把每个约束方向写成一个固定的支持矩阵 `S_c^{i→j} ∈ {0,1}^{M×M}`。

2) **值空间统一**：用统一值空间 `U={0..M-1}` 表示域（必要时对不同变量做 padding；不存在的值位恒为 0）。

3) **AC 的含义**：指“对所有约束反复应用 revise 直到不动点”（队列式/迭代式均可）。  
   本文把“单轮 revise 的支持判定”写成 bitGEMV/bitGEMM；完整 AC 是若干次这类判定的迭代组合。

---

## 1. 概念回顾：AC / SAC-checking / SAC-closure

### 1.1 Arc Consistency (AC)

对任意二元约束 `c(X_i,X_j)` 和任意值 `a∈D_i`，存在某个 `b∈D_j` 使得 `(a,b)∈R_c`。否则从 `D_i` 中删掉 `a`。对所有约束重复直到不动点。

### 1.2 Singleton Arc Consistency (SAC)

SAC 的定义是一个“性质”（property）：

- 对每个变量–取值对 `(X_i,a)`，构造“单例世界”：把 `D_i` 强制收缩为 `{a}`，其它变量域不变；
- 在该世界中运行 AC 到不动点；
- 若出现空域（DWO），则 `(X_i,a)` 在原问题中不是 SAC-consistent，需要删除。

### 1.3 关键澄清：SAC-checking pass ≠ SAC-closure

给定当前域 `D`，做一次 “对所有 `(X_i,a)` 的 singleton-check” 并删除失败值，记为算子 `F_SAC(D)`。  
要得到满足 SAC 性质的闭包，需要迭代：

`D ← F_SAC(D)` 直到 `D` 不再变化。

你想用 GPU “batch 化”做的，最自然的对象是 `F_SAC` 这一轮（一次 pass）。这也是下文“batched AC = SAC”最严谨的表述。

---

## 2. 你当前编码下的 AC 支持判定：一个 boolean bitGEMV

压缩符号，强调“线代形式”：

- 变量集合 `V={X_1..X_n}`，统一值空间 `U={0..M-1}`。
- 域编码：当前状态下 `X_i` 的域 `D_i⊆U` 用 bit 向量表示：
  - `d_i ∈ {0,1}^M`，其中 `d_i[v]=1 ⇔ v∈D_i`。
- 对二元约束 `c=(X_i,X_j)`，从方向 `i→j` 的支持矩阵：
  - `S_c^{i→j} ∈ {0,1}^{M×M}`，`S_c^{i→j}(a,b)=1 ⇔ (a,b)∈R_c`。

对固定约束 `c=(X_i,X_j)`，判断 `(a∈D_i)` 是否在 `D_j` 中有支持：

`g_{i,c}[a] = OR_{b=0..M-1} ( S_c^{i→j}(a,b) AND d_j[b] )`

这恰好是布尔半环 `({0,1}, OR, AND)` 下的矩阵–向量乘：

`g_{i,c} = S_c^{i→j} ⊙ d_j`

然后对所有相关约束取 AND 回写 `D_i`（这里只写“支持掩码”这一层，完整 AC 仍需迭代到不动点）：

`d_i' = d_i AND (AND_{c∈N(i)} g_{i,c})`

所以你现在的 “support row × domain bit-vector” 本质上就是一次 **boolean bitGEMV**（矩阵一行 bitset 与域 bitset 的相交+归约）。

---

## 3. “batched AC = SAC-checking pass”的语义等价

### 3.1 把一批单例世界打包成一个 batch CSP

假设我们要同时检查 `K` 个单例赋值：

`(X_{i_1}=a_1), (X_{i_2}=a_2), ..., (X_{i_K}=a_K)`

对每个 `k`，构造一个单例世界（一个子问题）`P^{(k)}`：

- 初始域：
  - `D_{i_k}^{(k,0)} = {a_k}`
  - `D_{t}^{(k,0)} = D_t^{(0)}`（`t≠i_k`）
- 约束集合与原问题相同（复制一份给该世界）。

把所有世界的变量与约束做“互不相交的并集”，得到一个批量问题 `P*`。  
**关键观察**：不同世界之间没有任何跨世界约束，所以它们完全独立。

### 3.2 语义等价结论（最重要）

在 `P*` 上运行 AC 到不动点，等价于对每个世界 `P^{(k)}` 分别运行 AC 到不动点。  
因此：

- 世界 `k` 在 AC 后产生空域  
  `⇔` singleton-check `(X_{i_k}=a_k)` 失败  
  `⇔` 在 `F_SAC(D)` 这一轮中应删除该值。

这就是“batched AC 能实现一轮 SAC-checking pass”的严格含义。

---

## 4. 算子层：把 batched AC 写成 boolean bitGEMM

把每个世界 `k` 的域向量堆叠起来：

- 对固定变量 `X_j`，把它在所有世界上的域拼成矩阵：
  - `D_j ∈ {0,1}^{M×K}`，`D_j[b,k]=1 ⇔ b∈D_j^{(k)}`

支持矩阵 `S_c^{i→j}` 不随世界变化，仍是 `M×M`。  
则在一轮“支持判定”里，批量世界的结果是：

`G_{i,c} = S_c^{i→j} ⊙ D_j   ∈ {0,1}^{M×K}`

其中（布尔半环的矩阵–矩阵乘）：

`G_{i,c}[a,k] = OR_{b=0..M-1} ( S_c^{i→j}(a,b) AND D_j[b,k] )`

直观解释：对每个 `a`，一次性得到它在所有世界 `k` 上是否有支持。  
当 `K=1` 时，上式退化回第 2 节的 bitGEMV。

因此，从算子视角可以说：

- 单世界 AC 的“支持判定”是 boolean bitGEMV  
- 批量世界的“支持判定”是 boolean bitGEMM  

这给了你一个很自然的 GPU 叙述：SAC-checking pass 可以看成把大量 bitGEMV 合并成一个更“像 GEMM”的批量算子。

---

## 5. 工程落地：两种“batch”的实现边界（必须讲清楚）

你想要的“GPU 做 bitGEMM”有现实的内存/元数据代价。实践上建议把 batch 分成两种：

### 5.1 Batch-1（时间维 batch，近期可落地）

核心思想：不同时存放多个世界，只是把很多 probe 任务**连续喂给同一个长驻/持久化传播内核**，从而摊薄 launch latency 与 host 调度开销。

- 仍是单世界域表示 `bitDom[var][word]`。
- 每个 probe：
  - snapshot（或 trail 回滚）恢复
  - 临时单例赋值
  - GPU 执行一次/多次 GAC 迭代到不动点
  - 恢复
- “batch”体现在：一次 kernel/一次持久化 kernel 连续处理很多 probe，而不是每个 probe 一个 kernel。

优点：内存需求低、最贴合 Jetson；实现成本相对可控。  
缺点：算子更像“很多 GEMV 串起来”，复用率有限，但仍可显著减少 CPU 负载与 kernel 启动开销。

### 5.2 Batch-2（空间维 micro-batch，长期方向）

核心思想：同时维护 `B` 个世界（`B` 是 micro-batch 大小），在 GPU 上并行跑这 `B` 个世界的 AC 到不动点；这更接近“真正的 bitGEMM”。

**关键现实**：`K=∑|D_i|` 通常很大，不可能把所有 singleton 世界一次性塞进显存/统一内存；必须 micro-batch。

内存粗估（只算域，不含队列/元数据）：

`bytes(bitDom_batch) ≈ B * num_vars * bit_dom_int_size * sizeof(uint32)`

还需要额外的 per-world 元数据（域大小缓存、队列/激活集合等）。因此 `B` 往往只能是几十到几百的量级（取决于实例规模与 Jetson 内存余量）。

优点：支持矩阵 `S` 在 `B` 个世界上复用，算子更接近 GEMM；有机会提高算术强度。  
缺点：实现复杂、内存压力大，还需要为每个世界维护传播状态（否则无法到不动点）。

---

## 6. 伪代码修正：micro-batch 下的“列并行”写法（避免维度混淆）

下面给一个不会混淆维度的伪代码（世界按列存储；每个世界的域仍是“值 word”数组）：

```cpp
// 输入:
// - sup_row[w]: 支持矩阵 S(a, :) 在第 w 个 value-word 的 32bit
// - dom_y[k][w]: 第 k 个世界里变量 y 的域，在第 w 个 value-word 的 32bit
// 输出:
// - supported[k]: 在第 k 个世界里，值 a 是否在 y 的当前域中找到任意支持
bool supported[B];
for (int k = 0; k < B; ++k) supported[k] = false;

for (int w = 0; w < bit_dom_int_size; ++w) {
  uint32_t sup = sup_row[w];
  for (int k = 0; k < B; ++k) {
    supported[k] = supported[k] || ((sup & dom_y[k][w]) != 0);
  }
}
```

说明：
- 这里 `w` 是“值空间”维度的 word 索引（每个 word 覆盖 32 个值）。
- `k` 是世界索引（micro-batch 内的列）。  
- 这等价于对 `B` 列同时做 `S ⊙ D_j` 的一行/一个值 `a` 的支持判定（也就是 GEMM 的一小块）。

如果你想把“世界维度”再做 bit-pack（32 个世界压成一个 32-bit mask），那需要对 `dom_y` 做转置/位切片（实现复杂，属于 Batch-2 的进一步优化，不建议在第一版就做）。

---

## 7. 回到你的直觉：GPU 为什么“更像适合做 SAC”

你的直觉“GPU 擅长 GEMM，所以 GPU 适合做 batched SAC”可以这样严谨表述：

- 从**算子**看：把很多 singleton-check 的支持判定合并，确实从 bitGEMV 走向 bitGEMM。  
- 从**性能**看：能否变快取决于 `B` 的大小与数据复用（support 行被多少世界共享），以及域/支持的内存访问是否能做到 coalesced。  
- 从**工程**看：Jetson 上最先落地的是 Batch-1（时间维 batch + 持久化传播）；Batch-2（空间维 micro-batch，近似 bitGEMM）是更长期、需要仔细算内存账的方向。

---

## 参考

- https://arxiv.org/abs/1704.06215
