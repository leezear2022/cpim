# CLAUDE.md

## 项目概述

CPIM 是 CUDA 加速的约束满足问题 (CSP) 求解器，实现了多种弧一致性算法 (AC3, FC, SAC, RPC 等)，支持 CPU 和 GPU 求解。解析 XCSP3 格式问题文件，使用 MAC 搜索算法求解。

## 构建与运行

```bash
cd build && cmake .. && make -j4

# CPU 求解器 (已验证的基线版本)
./cpim_test_parser --input=../samples/bench/queens-4_ext.xml

# 验证工具
./verify_gac --input=<file>      # 验证 AC3bit GAC 正确性
./verify_search --input=<file>   # 验证 MAC 搜索正确性
```

**依赖**: CUDA 11.0+, libxml2, Xerces-C, gflags, glog, Abseil

## 核心架构

详见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)

| 层 | 组件 | 说明 |
|---|------|------|
| 模型 | HModel, IntermediateModel | 约束模型表示 |
| 网络 | Network, IntVar, Tabular | 运行时网络，多级域 |
| 算法 | AC3bit, FC, SAC, MAC | 一致性和搜索算法 |
| GPU | CModel, GModel | GPU 加速求解 |

**数据流**: `XCSP3 → Parser → Model → Network → MAC + AC算法 → 解`

## 测试与验证

详见 [docs/TESTING_GUIDE.md](docs/TESTING_GUIDE.md)

- `verify_gac`: 验证 AC3bit GAC 传播正确性
- `verify_search`: 验证 MAC 搜索过程正确性

## 已修复的 Bug

| # | 问题 | 位置 | 状态 |
|---|------|------|------|
| 1 | get_solution() 使用错误索引 | src/MAC.cpp:255 | ✅ |
| 2 | UNSAT 时无条件调用 get_solution | samples/main_new_parser.cpp | ✅ |
| 3 | 找到解后未设置 num_sol | src/MAC.cpp:134 | ✅ |

## 关键文件

- `samples/main_new_parser.cpp` - CPU 求解器入口
- `src/MAC.cpp` - MAC 搜索实现
- `src/AC3bit.cpp` - AC3bit 算法
- `include/Network.h` - 多级域管理
- `src/GModel.cu` - 简化 GPU 模型

## 现代化计划

详见 `MODERNIZATION_MEMO.md` (快速参考) 和 `MODERNIZATION_PLAN_V2.md` (详细计划)

目标: 模块化架构、Trail 回溯、Fluent API、80%+ 测试覆盖

## 注意事项

- **通信语言**: 全程使用中文交流
- **cpim_test_parser**: 已验证的 CPU 基线版本
- **测试实例**: `samples/bench/`, `benchmarks/`
