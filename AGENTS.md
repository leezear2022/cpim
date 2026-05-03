# AGENTS.md

> Entry point for AI coding assistants (Codex, Gemini, etc.). Full project guide: [INSTRUCTIONS.md](INSTRUCTIONS.md).

## Quick Reference

- **Build**: `mkdir -p build && cd build && cmake .. && make -j$(nproc)`
- **CPU Solver**: `./cpim_test_parser --bench_path=<file>`
- **GPU Solver**: `./gmodel_solver --input=<file>`
- **Test**: `python3 tests/python/batch_test_v2.py --tier=0`

## Documentation

| Category | Document |
|----------|----------|
| **Full Guide** | [INSTRUCTIONS.md](INSTRUCTIONS.md) |
| Architecture | [docs/architecture/ARCHITECTURE.md](docs/architecture/ARCHITECTURE.md) |
| Applications | [docs/guides/APPS_REFERENCE.md](docs/guides/APPS_REFERENCE.md) |
| Testing | [docs/guides/TESTING_GUIDE.md](docs/guides/TESTING_GUIDE.md) |
| Doc Navigation | [docs/README.md](docs/README.md) |

## Code Style

- C++17 with RAII and Abseil
- Two-space indent, same-line braces, ≤100 char lines
- Types: `PascalCase`, functions: `lower_snake_case`, constants: `ALL_CAPS`
- Error handling: `absl::Status` / `StatusOr`
- CUDA: annotate host/device boundaries

## Commit Guidelines

- Concise subject (≤60 chars), action-oriented
- Mandarin phrasing matching existing history is fine
- Describe algorithmic intent in body
- Format:
  ```
  <type>: <summary>

  <details>

  Co-Authored-By: <AI Model> <noreply@...>
  ```

## Target Platform

**Jetson Orin Nano Super 8G**
- Ubuntu 22.04.5 LTS (kernel 5.15.148-tegra)
- ARM Cortex-A78AE 6-core
- NVIDIA Orin GPU, CUDA 12.6
- 7.4 GiB unified memory

## Development Guidelines (SAC-GPU Iteration)

- **Regression Test**: Run `batch_test_v2.py --tier=0` after each improvement to verify correctness
- **Performance Recording**: Record p50/p95/p99 times before and after changes to evaluate gains
- **Ablation Switches**: Keep runtime switches for key legacy versions (e.g., `--use_legacy_xxx`) for ablation experiments
- **Rollback Principle**: Any new path must be able to fall back to the stable Stage2 path

## Notes

- **Communication**: 全程使用中文交流 (Use Chinese throughout)
- **Changelog**: Update `CHANGES_ZH.md`
- **Main Programs**: `apps/cpim_test_parser.cpp` (CPU), `apps/gmodel_solver.cpp` (GPU)


## DocOps Logic

Read first:
- .docops/s.md
- .docops/c.yaml
- last 20 lines of .docops/k.jsonl

Before handoff:
- dol lint --soft
