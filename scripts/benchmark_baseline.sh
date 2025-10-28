#!/usr/bin/env bash
set -euo pipefail

if [[ ! -x "build/cpim" ]]; then
  echo "Error: build/cpim not found; please run cmake --build first." >&2
  exit 1
fi

BENCH_ROOT="samples/bench"
BENCHMARKS=(
  "${BENCH_ROOT}/queens-4_ext.xml"
  "${BENCH_ROOT}/queens-12_ext.xml"
  "${BENCH_ROOT}/haystacks-11_ext.xml"
)

OUTPUT_DIR="baseline_results"
mkdir -p "${OUTPUT_DIR}"
SUMMARY="${OUTPUT_DIR}/summary.txt"
: > "${SUMMARY}"

for bench in "${BENCHMARKS[@]}"; do
  if [[ ! -f "${bench}" ]]; then
    echo "Warning: ${bench} not found, skipping." >&2
    continue
  fi
  name=$(basename "${bench}" .xml)
  log="${OUTPUT_DIR}/${name}.log"
  echo "Running ${bench}..."
  /usr/bin/time -v ./build/cpim "${bench}" 2>&1 | tee "${log}"
  grep -E "(Elapsed time|GPU Statistics|GAC_success)" "${log}" >> "${SUMMARY}" || true
  echo "---" >> "${SUMMARY}"
 done

echo "Baseline logs written to ${OUTPUT_DIR}" 
