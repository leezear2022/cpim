# Batch AC GPU 设计与实施对比备忘

对比文档：
- 设计：`docs/planning/BATCH_AC_GPU_DESIGN.md`
- 实施：`docs/implementation/BATCH_AC_GPU_IMPLEMENTATION.md`

## 综合评语

两份文档在“Batch-1 优先、复用现有 GAC kernel、Cooperative Groups 持久化
内核”的主干方向上高度一致，实施文档把 Batch-1 的数据结构与 kernel 逻辑
拆得很细，适合直接落地；设计文档则覆盖了 AC-GPU 优化点与 Batch-2 的长期
方向，具备更完整的路线图。当前最大风险在于实现文档对状态恢复与回退策略
的描述不充分（`bitDom`/`d_cur_dom_size`/frontier 缓冲的恢复与清理），且与
现有 GModelSolver 的增量传播路径尚未统一口径，建议先补齐这些一致性细节
再进入实现阶段。

## 主要一致点

- 都将 Batch AC 等价为 SAC-checking pass 的一次批处理。
- Batch-1 作为首选路径，强调“时间维 batch + 持久化 kernel”。
- 复用 `ExecuteConstraintCheck_BpC`、`FetchNextCidFromBitmap`、
  `PropagateVarToNextBitmap` 的实现思路一致。
- 依赖统一内存与 Cooperative Launch，适配 Jetson Orin。

## 关键差异与补齐项

1) **状态恢复范围不一致**  
实施文档强调“快照恢复”，但仅对 `bitDom` 做恢复；  
设计文档强调“最小侵入”。  
建议补齐：每个 probe 需同步恢复 `d_cur_dom_size`（至少恢复全部变量的
域大小），并在 batch 结束后恢复全局状态，避免污染外部求解流程。

2) **frontier 清理时机**  
实施文档的 `InitializeFrontierForVariable` 只清空 `frontier_A`，未明确
`frontier_B` 的初始清零；  
设计文档提到 frontier 管理但未写细节。  
建议在每个 probe 开始前显式清零 `frontier_B`，避免跨 probe 污染。

3) **回退与兼容路径**  
实施文档在不支持 cooperative launch 时直接抛错；  
设计文档建议回退到非持久化或单世界路径。  
建议补齐 fallback：`EnforceGAC_Persistent` → `EnforceGAC` → CPU loop。

4) **与现有 GModelSolver 的接口衔接**  
设计文档建议优化 `GModelSolver` 调用（增量 frontier + 持久化）；  
实施文档直接引入 `BatchProbeManager` 与 `MSAC_GPU`。  
建议统一：先在 `GModelSolver` 路径验证增量传播收益，再引入 Batch-1
probe 管理器，避免双线并行导致维护成本。

5) **配置参数暴露**  
设计文档提到 `micro_batch/max_iters/use_persistent`；  
实施文档固定 `maxIterations = num_vars * 10`。  
建议统一为可配置参数并记录到统计结构中。

## 风险清单（落地前需验证）

- **域快照恢复完整性**：`bitDom` + `d_cur_dom_size` 是否一致？
- **probe 后状态污染**：批处理结束后是否恢复基准域状态？
- **frontier 脏数据**：不同 probe 之间是否存在位图残留？
- **性能预期**：性能数据为假设值，需在 `dump_gmodel` 或独立测试中实测。

## 建议的下一步

1) 在实施文档中补齐状态恢复/清理与 fallback 细节，并标注与
   `GModel::EnforceGAC(_Persistent)` 的关系。
2) 先把 `GModelSolver` 的 GAC 调用改为“增量 frontier + 持久化 kernel”
   进行基线性能验证。
3) 以 Queens-4/Queens-12 为 TIER 0 样例验证 Batch-1 正确性，
   再扩展到 Langford 系列。
