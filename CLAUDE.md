# CLAUDE.md

> Claude Code 专用入口。完整项目指南见 [INSTRUCTIONS.md](INSTRUCTIONS.md)。

## 快速参考

- **构建**: `mkdir -p build && cd build && cmake .. && make -j4`
- **CPU 求解**: `./cpim_test_parser --bench_path=<file>`
- **GPU 求解**: `./gmodel_solver --input=<file>`
- **测试**: `python3 tests/python/batch_test_v2.py --tier=0`

## 文档导航

| 类别 | 文档 |
|------|------|
| **完整指南** | [INSTRUCTIONS.md](INSTRUCTIONS.md) |
| 系统架构 | [docs/architecture/ARCHITECTURE.md](docs/architecture/ARCHITECTURE.md) |
| 应用程序 | [docs/guides/APPS_REFERENCE.md](docs/guides/APPS_REFERENCE.md) |
| 测试指南 | [docs/guides/TESTING_GUIDE.md](docs/guides/TESTING_GUIDE.md) |
| 文档导航 | [docs/README.md](docs/README.md) |

## 开发状态

**当前阶段**: Phase 1.5 完成（变量启发式）

| Phase | 状态 |
|-------|------|
| 1.1 统一 Trail | ✅ |
| 1.2 GPU 搜索验证 | ✅ |
| 1.5 变量启发式 | ✅ |
| 1.3 自适应切换 | 待实施 |
| 2 Propagator 框架 | 待实施 |

规划文档：[docs/planning/MODERNIZATION_PLAN_V2.md](docs/planning/MODERNIZATION_PLAN_V2.md)

## 注意事项

- **通信语言**: 全程使用中文交流
- **变更日志**: 更新 `CHANGES_ZH.md`
- **主程序**: `apps/cpim_test_parser.cpp`（CPU）, `apps/gmodel_solver.cpp`（GPU）
