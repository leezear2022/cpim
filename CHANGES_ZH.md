# 修改清单（中文）
## 2025-12-17
- 新增文档 `benchmarks/README.md`：对 `benchmarks/` 基准库做走读，汇总子目录样例类型与数量，说明当前解析器对 `*_ext.xml`/`*-ext.xml`（表约束、supports/conflicts）的支持边界，并给出 `cpim_test_parser`/`dump_gmodel` 的推荐使用方式与过滤建议。

## 2025-10-21
- 新增文档：`aig_docs/GPU_GAC_UNIFIED_MEMORY_GUIDE.md`
  - 总结 Jetson Orin（统一内存 UMA）下的约束传播优化方案：统一内存单源数据、事件驱动、CPU/GPU 动态调度、持久化 kernel、设备侧队列与按字位集处理等；给出代码落点与渐进落地步骤，并关联 `GPU_GAC_PERSISTENT_STATE_MACHINE.md` 与 `GPU_GAC_PIPELINE_PLAN.md`。

- 调整 `include/model/xcsp_parser.h`，新增基准路径元数据结构与接口，以便统一处理单个文件、目录与清单。
- 更新 `include/model/libxml2_parser.h` 与 `src/model/libxml2_parser.cpp`，重写清单解析与路径归一化逻辑，支持 XCSP2 文件格式识别，并补充目录遍历、格式探测等辅助函数。
- 扩展 `samples/main_new_parser.cpp`，集成 Abseil Flags 命令行参数，支持列出基准文件、选择清单或直接路径，并输出检测到的 XCSP 版本。
- 在 `CMakeLists.txt` 中为 `cpim_test_parser` 目标链接 `absl::flags` 与 `absl::flags_parse`，满足新命令行功能的依赖。
- 新增 `CHANGES_ZH.md`（本文件），提供近期变更的中文汇总。
- 新增文档 `aig_docs/GPU_GAC_PERSISTENT_STATE_MACHINE.md`，提出基于“持久化内核 + 约束状态机 + 设备端队列”的 GAC 传播方案，并与 `aig_docs/GPU_GAC_PIPELINE_PLAN.md` 对比评估 Jetson Orin 适配性。

- 新增 CPU 基线传播工具：`cpim_gac_cpu`
  - 位置：`samples/run_gac_cpu.cpp`, `include/model/gac_cpu.h`, `src/model/gac_cpu.cpp`
  - 功能：串行 CPU 版队列压缩与 GAC 传播（仅二元 supports 约束），用于正确性基线与后续 GPU 优化对照。
  - 构建与运行：
    - 构建：`cmake -S . -B build_orin -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build_orin -j$(nproc)`
    - 运行：`./build_orin/cpim_gac_cpu --input=samples/bench/queens-4_ext.xml --max_print=8`

- GModel GPU 基线传播实现：`GModel::EnforceGAC`
  - 位置：`include/GModel.cuh`, `src/GModel.cu`, `src/model/gmodel_adapter.cu`, `samples/dump_gmodel.cpp`
  - 功能：参考 `cuSAC.cu` 的 `enforceGAC` 流程，在统一内存 `GModel` 上实现 CPU 串行队列 + GPU `CsCheckMain` kernel 的 GAC 传播，并引入共享内存缓存与按字节掩码归约，输出迭代次数、删除数及是否检测到不一致。
  - 使用方式：`./build_orin/dump_gmodel --input=... --run_gac`。
