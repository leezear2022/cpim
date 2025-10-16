# Repository Guidelines

## Project Structure & Module Organization
The solver core lives in `src/`, mixing CPU algorithms (`AC3.cpp`, `MAC.cpp`, `FC.cpp`) with GPU kernels (`cuSAC.cu`) and the modern parser stack under `src/model/`. Public interfaces stay mirrored in `include/`. XCSP2 benchmarks and manifests sit in `samples/bench/`; store new `.xml` cases there and keep `BMPath.xml` style manifests close to their batches. Use `build/` for generated artifacts; avoid touching `deprecated/` and `xcsp3parser/` unless migrating legacy logic.

## Build, Test, and Development Commands
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` for a clean Release configure; add `-DCMAKE_CUDA_ARCHITECTURES="60;70;80"` when tuning GPU binaries.
- `cmake --build build -j$(nproc)` builds every target, including the parser test harness.
- `./build/cpim_test_parser --list_only` prints resolved bench files and their XCSP format; add `--bench_path samples/bench` to scan an entire directory or `--bench_manifest samples/bench/BMPath.xml` for curated manifests.
- `./build/cpim_test_parser` (without flags) parses the first discovered XCSP2 instance and dumps stats for smoke validation.

## Coding Style & Naming Conventions
Write Modern C++17 with RAII and Abseil helpers where useful. Follow the prevailing two-space indentation, same-line braces, and keep lines under roughly 100 characters. Use `PascalCase` for types, lower_snake_case for functions that mirror STL semantics (`push_back`), and ALL_CAPS for constants. Prefer `absl::Status` / `StatusOr` over raw error codes, and document host/device boundaries in CUDA code when behavior diverges.

## Testing Guidelines
Lean on `cpim_test_parser` for quick regression checks: point it at new manifests or individual `.xml` files and confirm the reported XCSP format matches expectations. When adding unit suites, wire GoogleTest through CMake and register with `add_test` so `ctest --output-on-failure` under `build/` exercises them. Log solver runtime or memory shifts in merge notes whenever propagation loops or CUDA kernels change.

## Commit & Pull Request Guidelines
Keep commit subjects concise (≤60 characters) and action oriented; Mandarin phrasing that matches the existing history is fine. Group related edits together, describe algorithmic intent in the body, and attach parser/solver output snippets or performance deltas for non-trivial changes. Pull requests should call out touched areas (`src/model`, `samples/bench`, etc.) and link tracked issues when applicable.

## Documentation & Collaboration
Refer to `CHANGES_ZH.md` for the latest中文修改清单，并在该文件中持续更新新增改动。全程用中文交流。
