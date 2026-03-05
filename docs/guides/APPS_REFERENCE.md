# CPIM 应用程序参考

本文档列出 CPIM 项目中所有可执行程序和测试脚本。

---

## C++ 应用程序

### 求解器

#### cpim_test_parser

**CPU 求解器主程序**

```bash
./cpim_test_parser --bench_path=<XCSP3文件>
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--bench_path` | XCSP3 问题文件路径 | 必填 |
| `--ac_algo` | AC 算法 (AC3bit/RPC3/lMaxRPC) | AC3bit |
| `--heuristic` | 变量选择启发式 | min_domain |

支持的启发式：`min_domain`, `dom_deg`, `dom_ddeg`

#### gmodel_solver

**GPU 求解器**

```bash
./gmodel_solver --input=<XCSP3文件> [选项]
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--input` | XCSP3 问题文件路径 | 必填 |
| `--heuristic` | 变量选择启发式 | min_domain |
| `--verbose` | 详细输出 | false |

---

### 验证工具

#### verify_gac

**验证 AC3bit GAC 传播正确性**

```bash
./verify_gac --input=<XCSP3文件>
```

验证传播后所有约束的弧一致性。

#### verify_search

**验证 MAC 搜索过程正确性**

```bash
./verify_search --input=<XCSP3文件>
```

验证搜索中的变量选择、值移除和回溯操作。

---

### 基准测试

#### sac_benchmark

**SAC-GPU 基准测试**

```bash
./sac_benchmark --input=<XCSP3文件> [选项]
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--input` | XCSP3 问题文件路径 | 必填 |
| `--test_full_sac` | 测试完整 SAC 收敛 | false |
| `--test_msac` | 测试 MSAC 模拟 | false |
| `--num_assignments` | MSAC 模拟的赋值数 | 3 |
| `--msac_mode` | MSAC 模式 (fast/full/parallel) | fast |

#### benchmark_probe_throughput

**Batch 探测吞吐量对比**

```bash
./benchmark_probe_throughput --input=<XCSP3文件>
```

对比 Batch2 和 Batch3 的探测吞吐量。

#### compare_cpu_gpu

**CPU/GPU 求解器对比**

```bash
./compare_cpu_gpu --input=<XCSP3文件>
```

对比 CPU 和 GPU 求解器的性能和正确性。

---

### 调试工具

#### dump_gmodel

**GModel 状态导出**

```bash
./dump_gmodel --input=<XCSP3文件>
```

导出 GModel 的数据结构到标准输出，用于调试。

#### cpim_dump

**模型导出**

```bash
./cpim_dump --bench_path=<XCSP3文件>
```

导出解析后的约束模型信息。

#### cpim_gac_cpu

**CPU GAC 工具**

```bash
./cpim_gac_cpu --bench_path=<XCSP3文件>
```

使用 CPU 运行 GAC 传播，用于验证和调试。

---

## Python 测试脚本

位于 `tests/python/` 目录。

### 批量测试

#### batch_test_v2.py

**分层批量测试**（主测试脚本）

```bash
python3 tests/python/batch_test_v2.py --tier=<0|1|2>
```

| Tier | 说明 | 实例规模 |
|------|------|----------|
| 0 | 快速验证 | 小实例（Queens-4/12 等） |
| 1 | 中等规模 | Langford, rand-2-* 等 |
| 2 | 大规模 | 完整 benchmarks/ |

配置文件：[tier_definitions.py](../../tests/python/tier_definitions.py)

---

### 对比测试

#### compare_cpu_gpu.py

**CPU/GPU 节点数对比**

```bash
python3 tests/python/compare_cpu_gpu.py [--tier=<0|1|2>]
```

验证 CPU 和 GPU 求解器的节点数一致性。

#### compare_ac_algorithms.py

**AC 算法对比**

```bash
python3 tests/python/compare_ac_algorithms.py
```

对比 AC3bit, RPC3, lMaxRPC 在相同问题上的表现。

#### compare_sac_algorithms.py

**SAC 算法对比**

```bash
python3 tests/python/compare_sac_algorithms.py
```

对比 SAC1, SAC3 的性能。

#### compare_activation_strategies.py

**激活策略对比**

```bash
python3 tests/python/compare_activation_strategies.py
```

对比不同 GPU 激活策略的效果。

#### compare_batch2_tier0.py

**Batch2 TIER 0 测试**

```bash
python3 tests/python/compare_batch2_tier0.py
```

在 TIER 0 实例上测试 Batch2 实现。

---

### 启发式基准

#### benchmark_heuristics.py

**变量选择启发式对比**

```bash
python3 tests/python/benchmark_heuristics.py [--tier=<0|1|2>]
```

对比 MinDomain, DOM/DEG, DOM/DDEG 的搜索节点数。

---

### 外部求解器

#### solve_xcsp_ortools.py

**OR-Tools 求解器（SAT 后端）**

```bash
python3 tests/python/solve_xcsp_ortools.py <XCSP3文件>
```

使用 Google OR-Tools SAT 求解器求解，用于结果验证。

#### solve_xcsp_ortools_cp.py

**OR-Tools 求解器（CP 后端）**

```bash
python3 tests/python/solve_xcsp_ortools_cp.py <XCSP3文件>
```

使用 Google OR-Tools CP 求解器求解。

---

## 测试数据

| 目录 | 说明 |
|------|------|
| `tests/data/bench/` | 小型测试实例（Git 托管） |
| `benchmarks/` | 大型测试实例（需单独下载） |

---

## 相关文档

- [测试指南](TESTING_GUIDE.md) - 完整测试流程
- [分层测试定义](../../tests/python/tier_definitions.py) - TIER 配置
