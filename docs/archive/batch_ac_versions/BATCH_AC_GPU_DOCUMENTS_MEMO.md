# Batch AC GPU 文档体系备忘

## 文档元数据

- **创建时间**: 2025-12-25
- **目的**: 总结 Batch AC GPU 相关文档的定位、差异和使用建议
- **状态**: 活跃文档

---

## 文档体系概览

当前项目存在三个核心文档，形成完整的"理论 → 设计 → 实施"链条：

```
[理论基础]
   ↓
docs/planning/Batch_AC.md (理论框架，SAC=Batch AC的数学证明)
   ↓
[概念设计]
   ↓
docs/planning/BATCH_AC_GPU_DESIGN.md (工程化设计，GModel集成路径)
   ↓
[实施指南]
   ↓
docs/implementation/BATCH_AC_GPU_IMPLEMENTATION.md (代码级实施文档)
```

---

## 文档对比分析

### 1. BATCH_AC_GPU_DESIGN.md（概念设计）

**定位**: 从现有 GModel 出发的工程化设计方案

**特点**：
- ✅ **简洁高效**（154行，适合快速理解）
- ✅ **紧密贴合现状**（基于当前 GModel.cu/GModelSolver.cu 的实现细节）
- ✅ **优化空间分析**（6点 AC-GPU 优化建议，部分可独立实施）
- ✅ **方案对比清晰**（Batch-1 vs Batch-2 的权衡分析）
- ✅ **风险与回退**（内存压力、设备能力、实现复杂度）

**核心价值**：
1. **现状诊断**（第3节）：
   - 增量 frontier 未启用
   - 持久化 kernel 可作为默认
   - frontier 非空检测可下沉到 GPU
   - bitSup 访问路径优化
   - 线程粒度调优
   - 域大小维护优化

2. **接口建议**（第6节）：
   - `BatchACOptions` 配置结构
   - `GModelBatchRunner` 封装类
   - 保留 `EnforceGAC` 单世界回退

3. **验证基准**（第7节）：
   - 与 CPU MSAC3bit 对照删值集合
   - `B` 对吞吐与内存的影响曲线

**适用场景**：
- 项目启动前的技术评审
- 向团队/用户介绍方案
- 快速查阅设计思路

---

### 2. BATCH_AC_GPU_IMPLEMENTATION.md（实施指南）

**定位**: 代码级实施文档，包含完整的数据结构、kernel 代码、测试计划

**特点**：
- ✅ **极度详细**（1483行，涵盖所有实施细节）
- ✅ **代码就绪**（数据结构、kernel、API 均有完整代码）
- ✅ **时间规划明确**（Day 1-2/3-4/5-7 等详细任务分解）
- ✅ **测试覆盖完整**（单元测试、集成测试、性能测试）
- ✅ **FAQ 丰富**（8个常见问题及解答）
- ✅ **内存/性能分析**（Queens-12/Langford-3-9 详细表格）

**核心价值**：
1. **数据结构设计**（第3节）：
   - ProbeTask（12 bytes，内存布局图）
   - BatchProbeControl（128 bytes，完整字段说明）
   - BatchProbeManager（API 设计 + 使用示例）

2. **Kernel 实现详解**（第4节）：
   - PersistentBatchProbeKernel（150行完整代码）
   - RunGACToFixpoint（80行，封装现有逻辑）
   - InitializeFrontierForVariable（30行）

3. **API 设计**（第5节）：
   - 公共接口（构造、AddTask、ExecuteBatch、Clear）
   - 内部方法（AllocateGPUMemory、LaunchBatchProbeKernel）
   - 完整错误处理和资源管理

4. **实施步骤**（第7节）：
   - Week 1: 核心实现（8天详细任务）
   - Week 2: 测试与集成（6天任务）
   - Week 3-4: 优化与扩展测试

**适用场景**：
- 实际编码时的参考手册
- Code review 的验收标准
- 新成员快速上手

---

## 关键差异对比表

| 维度 | DESIGN.md（概念设计） | IMPLEMENTATION.md（实施指南） |
|------|---------------------|----------------------------|
| **篇幅** | 154行 | 1483行 |
| **详细度** | 高层设计 | 代码级细节 |
| **代码量** | 伪代码片段 | 完整可用代码 |
| **时间规划** | 无 | Day 1-2, 3-4 等详细分解 |
| **测试计划** | 概述 | 8个详细测试用例 |
| **性能分析** | 概念性 | 详细表格（内存、时间） |
| **FAQ** | 无 | 8个常见问题 |
| **适用阶段** | 设计评审 | 实施编码 |
| **目标读者** | 技术决策者 | 工程师 |
| **更新频率** | 低（稳定设计） | 高（随实施调整） |

---

## 综合评语

### 优势互补

1. **设计文档的独特价值**：
   - ✅ **贴合现状**：从当前 GModel 实现出发，提出 6 点优化建议
   - ✅ **增量改进**：部分优化（如增量 frontier）可独立于 Batch AC 实施
   - ✅ **接口建议**：`GModelBatchRunner` 封装思路清晰，避免侵入核心
   - ✅ **风险意识**：明确提出内存压力、设备能力的回退策略

2. **实施文档的独特价值**：
   - ✅ **可执行性强**：数据结构、kernel、API 均有完整代码，可直接复制粘贴
   - ✅ **测试驱动**：8个测试用例覆盖单元、集成、性能三个层次
   - ✅ **性能量化**：Queens-12/Langford-3-9 的内存/时间详细分析
   - ✅ **FAQ 实用**：8个常见问题覆盖技术选型、故障排查、集成方式

### 潜在冲突与建议

#### 冲突 1：接口设计差异

**DESIGN.md 建议**（第6节）：
```cpp
class GModelBatchRunner {
  RunSACPass(const std::vector<std::pair<int,int>>& probes, ...);
  ApplyRemovals(...);
};
```

**IMPLEMENTATION.md 实际**（第3.3节）：
```cpp
class BatchProbeManager {
  AddTask(int var_id, int value);
  ExecuteBatch(std::vector<int>& failed_vars, std::vector<int>& failed_values);
};
```

**差异分析**：
- 设计文档建议"一次性传入所有 probes"（`RunSACPass`）
- 实施文档采用"逐个添加 + 批量执行"（`AddTask` + `ExecuteBatch`）

**建议**：
- ✅ **保留实施文档的接口**（`BatchProbeManager`）：更灵活，支持动态添加任务
- ⚠️ **考虑增加 `RunSACPass` 便捷接口**（可选）：
  ```cpp
  int BatchProbeManager::RunSACPass(
      const std::vector<std::pair<int,int>>& probes,
      std::vector<int>& failed_vars,
      std::vector<int>& failed_values) {
    for (auto [var, val] : probes) AddTask(var, val);
    return ExecuteBatch(failed_vars, failed_values);
  }
  ```

#### 冲突 2：优化建议的优先级

**DESIGN.md 提出的 6 点优化**（第3节）：
1. 使用增量 frontier（✅ 实施文档已采纳，见 InitializeFrontierForVariable）
2. 默认启用持久化 kernel（✅ 实施文档已采纳）
3. frontier 非空检测下沉到 GPU（✅ 实施文档已采纳，见 RunGACToFixpoint）
4. bitSup 访问路径与缓存策略（❌ 实施文档未涉及）
5. 线程粒度与 shared 复用（❌ 实施文档未涉及）
6. 域大小维护与回溯成本（❌ 实施文档未涉及）

**建议**：
- ✅ **优先级 1**（Batch AC 必需）：1-3 已完成
- 📋 **优先级 2**（独立优化项）：4-6 可在 Week 3-4 "性能优化"阶段实施
- 📝 **文档更新**：在实施文档第7节"Week 3: 性能优化"中补充 4-6 的实施细节

---

## 文档使用建议

### 阅读顺序（新成员）

1. **理论基础**（30分钟）：
   - 阅读 `Batch_AC.md` 第2-4节（核心论证）
   - 理解"SAC-checking pass = Batch AC"的数学等价性

2. **概念设计**（15分钟）：
   - 阅读 `BATCH_AC_GPU_DESIGN.md` 全文
   - 重点关注第3节（优化空间）和第4节（方案对比）

3. **实施细节**（2-3小时）：
   - 阅读 `BATCH_AC_GPU_IMPLEMENTATION.md`
   - Day 1-2: 第3节（数据结构）
   - Day 3-4: 第5节（API设计）
   - Day 5-7: 第4节（Kernel实现）

### 查阅场景

| 场景 | 推荐文档 | 关键章节 |
|------|---------|---------|
| 技术方案评审 | DESIGN.md | 第4节（方案对比）、第8节（风险） |
| 编码实现 | IMPLEMENTATION.md | 第3-5节（数据结构、Kernel、API） |
| 测试验证 | IMPLEMENTATION.md | 第8节（测试计划） |
| 性能调优 | IMPLEMENTATION.md | 第9节（性能分析） |
| 故障排查 | IMPLEMENTATION.md | 第10节（FAQ） |
| 优化现有 GAC | DESIGN.md | 第3节（AC-GPU 优化空间） |

---

## 实施建议

### Week 1-2（核心功能）

**执行文档**: `BATCH_AC_GPU_IMPLEMENTATION.md`
- 严格按照第7节的 Day 1-8 任务执行
- 验收标准：Queens-4 节点数匹配（P=4, N=0）

**参考文档**: `BATCH_AC_GPU_DESIGN.md`
- 遇到设计决策问题时参考第4.3节（Kernel 方案）
- 内存预算估算参考第4.2节

### Week 3-4（优化与测试）

**执行文档**: `BATCH_AC_GPU_IMPLEMENTATION.md`
- Week 3: 第7节"性能优化"
- Week 4: 第7节"大规模测试"

**补充任务**（来自 DESIGN.md 第3节）：
1. **bitSup 访问优化**（优先级：中）：
   - 测试 `__ldg` / `__restrict__` 对 bitSupData 的加速效果
   - 对比纹理路径（texObj_BitSup）的性能

2. **线程粒度调优**（优先级：低）：
   - 对大域实例（max_dom_size > 256）实验 word-level 并行
   - 评估 warp divergence 的改善

3. **域大小维护优化**（优先级：低）：
   - 记录"被修改变量集合 + 变更 delta"
   - 减少回溯时的全量 popcount

### 文档同步

**问题**：两个文档的部分内容会随实施演进

**建议**：
1. **DESIGN.md 保持稳定**：作为"设计快照"，不频繁修改
2. **IMPLEMENTATION.md 持续更新**：
   - 每周更新"实施步骤"的进度
   - 新增优化点（如 4-6）时补充到第7节
   - FAQ 随实际问题动态增删

3. **新增 CHANGELOG.md**（可选）：
   - 记录设计变更和实施决策
   - 追踪与设计文档的差异及原因

---

## 关键决策记录

### 决策 1：接口命名

**设计建议**: `GModelBatchRunner`
**实施选择**: `BatchProbeManager`

**理由**：
- "Manager" 更符合 RAII 资源管理语义
- "Probe" 明确语义（probe = singleton-check）
- 保留扩展性（未来可增加 `RunSACPass` 便捷接口）

**影响**: 无（纯命名差异，不影响功能）

---

### 决策 2：优化项分阶段实施

**设计建议**: 6点优化一起考虑
**实施选择**: 1-3 优先（Batch AC 核心），4-6 延后（独立优化）

**理由**：
- 1-3 是 Batch AC 必需（增量 frontier、持久化 kernel、收敛检测）
- 4-6 是 AC-GPU 通用优化（可独立于 Batch AC 实施）
- 降低首版复杂度，快速验证核心功能

**影响**: Week 3-4 需补充 4-6 的实施细节

---

### 决策 3：Batch-1 优先，Batch-2 暂缓

**共识**: 两个文档一致

**理由**（IMPLEMENTATION.md 第1.2节总结）：
- 开发周期：1-2周 vs 3-4月
- 实现复杂度：复用>95% vs 重写 kernel
- 内存需求：<10KB vs 10-100MB
- 收益：10-20x 已足够

**影响**: 无（一致决策）

---

## 后续工作

### 短期（Week 1-2）

- [ ] 按照 IMPLEMENTATION.md 第7节执行 Day 1-8 任务
- [ ] 每日更新进度到 IMPLEMENTATION.md（或新建进度跟踪文档）
- [ ] Queens-4 验证通过后更新 IMPLEMENTATION.md 的"验收标准"

### 中期（Week 3-4）

- [ ] 补充 DESIGN.md 第3节中 4-6 点优化的实施细节到 IMPLEMENTATION.md
- [ ] 性能测试后更新 IMPLEMENTATION.md 第9节的实际数据
- [ ] TIER 1 测试结果记录到 IMPLEMENTATION.md 第8节

### 长期（Phase 1.6 完成后）

- [ ] 考虑合并两个文档为单一"Batch AC GPU 完整指南"
- [ ] 或保持分离，但在 README 中明确各自定位
- [ ] 根据实施经验更新 Batch_AC.md 的理论部分（如有新发现）

---

## 总结

### 优势

1. **理论扎实**：Batch_AC.md 提供严谨的数学证明
2. **设计清晰**：DESIGN.md 紧密贴合现状，优化建议可独立实施
3. **实施详尽**：IMPLEMENTATION.md 代码就绪，测试完备
4. **互补完整**：三个文档形成"理论 → 设计 → 实施"闭环

### 建议

1. **保持文档分离**：各有侧重，避免冗余
2. **同步关键决策**：设计变更需双向更新
3. **持续迭代**：IMPLEMENTATION.md 随实施演进
4. **新增进度跟踪**：可选，用于记录实际进度与计划差异

### 风险

1. **文档不一致**：设计与实施偏离（已识别冲突 1-2，需协调）
2. **过度设计**：IMPLEMENTATION.md 过于详细可能导致维护负担
3. **优化遗漏**：DESIGN.md 的 4-6 点优化需在 Week 3-4 补充

---

**备忘状态**: 活跃（随项目进展更新）
**下次审查**: Week 2 结束（2025-12-30 预计）
**维护责任人**: Phase 1.6 GPU Batch AC Implementation Team
