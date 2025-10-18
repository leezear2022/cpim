# 修改清单（中文）

- 调整 `include/model/xcsp_parser.h`，新增基准路径元数据结构与接口，以便统一处理单个文件、目录与清单。
- 更新 `include/model/libxml2_parser.h` 与 `src/model/libxml2_parser.cpp`，重写清单解析与路径归一化逻辑，支持 XCSP2 文件格式识别，并补充目录遍历、格式探测等辅助函数。
- 扩展 `samples/main_new_parser.cpp`，集成 Abseil Flags 命令行参数，支持列出基准文件、选择清单或直接路径，并输出检测到的 XCSP 版本。
- 在 `CMakeLists.txt` 中为 `cpim_test_parser` 目标链接 `absl::flags` 与 `absl::flags_parse`，满足新命令行功能的依赖。
- 新增 `CHANGES_ZH.md`（本文件），提供近期变更的中文汇总。
 - 新增文档 `aig_docs/GPU_GAC_PERSISTENT_STATE_MACHINE.md`，提出基于“持久化内核 + 约束状态机 + 设备端队列”的 GAC 传播方案，并与 `aig_docs/GPU_GAC_PIPELINE_PLAN.md` 对比评估 Jetson Orin 适配性。
