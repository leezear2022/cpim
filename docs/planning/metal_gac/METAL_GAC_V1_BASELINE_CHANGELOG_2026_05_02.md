---
status: active
updated: 2026-05-03
type: changelog
scope: metal-gac
---

# Metal GAC v1.x Baseline Changelog

## 摘要

- Metal v1.x 完成 GAC correctness、runtime、benchmark、storage/frontier 消融、
  tier 扫描与 unsupported 分类。
- 该阶段目标是建立 Mac Metal GAC-only 后端的正确性和可测基础，不改变
  CUDA/Jetson stable path。

## 已落地能力

- 二元 extension GAC correctness。
- Metal runtime、kernel 编译与 benchmark 入口。
- `readonly_storage=shared|private` 消融。
- `frontier_mode=flags|compact` 消融。
- `metal_gac_ablation.py` tier 扫描。
- `metal_gac_analyze.py` 汇总与错误分类。

## 支持边界

- 支持：二元 extension constraints。
- 不纳入 v1/v2 性能支持面：
  - `global:allDifferent`
  - predicate/intension
  - 非二元 extension

这些 unsupported 类型必须明确分类，不计入 Metal GAC v2 性能失败。

## 决策

- 不把 `global:allDifferent` 展开为 pairwise `!=`，因为这不是 global
  AllDifferent GAC。
- parser/normalizer 遇到 unsupported 类型时返回明确错误，而不是崩溃或误归因。
- 默认 Metal 路径保持 off / 可回退，不影响 CUDA/Jetson。

## 后续依赖

- v2 性能线只能在二元 extension GAC 支持面内评估。
- SAC/Batch、global constraints、predicate/intension 单独进入后续计划。
