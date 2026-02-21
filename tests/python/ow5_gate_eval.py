#!/usr/bin/env python3
"""
OW5 gate evaluator.

Main gate (hard):
  - 9 medium/large cases
  - compare OW2 vs OW5(tile=8)
  - pass when median_ratio <= 1.00 and no unknown drift

Optional:
  - extra tile sweeps (4/16)
  - 3 stress cases for trend only (not hard gate)
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import pathlib
import re
import statistics
import subprocess
import sys
from dataclasses import dataclass
from typing import Dict, List, Optional


MAIN_CASES = [
    "benchmarks/tightness0.9/rand-2-40-180-84-900-11_ext.xml",
    "benchmarks/tightness0.9/rand-2-40-180-84-900-46_ext.xml",
    "benchmarks/tightness0.9/rand-2-40-180-84-900-25_ext.xml",
    "benchmarks/tightness0.9/rand-2-40-180-84-900-93_ext.xml",
    "benchmarks/composed-25-10-20/composed-25-10-20-6_ext.xml",
    "benchmarks/composed-25-10-20/composed-25-10-20-3_ext.xml",
    "benchmarks/composed-25-1-2/composed-25-1-2-4_ext.xml",
    "benchmarks/composed-75-1-2/composed-75-1-2-5_ext.xml",
    "benchmarks/QCP-15/qcp-15-120-14_ext.xml",
]

STRESS_CASES = [
    "benchmarks/driver/driverlogw-05c-sat_ext.xml",
    "benchmarks/driver/driverlogw-04c-sat_ext.xml",
    "benchmarks/langford/langford-3-11-ext.xml",
]


@dataclass
class BenchStats:
    avg_ms: float
    unknown: int
    line: str


def parse_fqpt_line(output: str) -> BenchStats:
    line = ""
    for candidate in output.splitlines():
        if "FQ-PT(" in candidate:
            line = candidate.strip()
    if not line:
        raise RuntimeError("No FQ-PT line found in benchmark output.")

    avg_ms_match = re.search(r"\)\s+([0-9]+(?:\.[0-9]+)?)\s+ms", line)
    unknown_match = re.search(r"\s([0-9]+)\s+unknown", line)
    if not (avg_ms_match and unknown_match):
        raise RuntimeError(f"Failed to parse benchmark line:\n{line}")
    return BenchStats(
        avg_ms=float(avg_ms_match.group(1)),
        unknown=int(unknown_match.group(1)),
        line=line,
    )


def run_once(
    bench_bin: pathlib.Path,
    case_path: str,
    mode: str,
    tile: int,
    num_probes: int,
    warmup: int,
    iterations: int,
    timeout_sec: int,
) -> subprocess.CompletedProcess[str]:
    cmd = [
        str(bench_bin),
        "--mode=fqpt",
        "--input",
        case_path,
        "--fqpt_enable_world_owner=1",
        "--fqpt_enable_world_stealing=1",
        "--fqpt_enable_cid_microbatch=0",
        "--fqpt_enable_subwarp_multiworld=1" if mode == "ow5" else "--fqpt_enable_subwarp_multiworld=0",
        f"--fqpt_subwarp_tile={tile}",
        f"--num_probes={num_probes}",
        f"--warmup={warmup}",
        f"--iterations={iterations}",
    ]
    return subprocess.run(
        cmd,
        text=True,
        capture_output=True,
        check=False,
        timeout=timeout_sec,
    )


def safe_ratio(numer: float, denom: float) -> float:
    if denom <= 0.0:
        return 0.0
    return numer / denom


def run_case_pair(
    bench_bin: pathlib.Path,
    case_path: str,
    tile: int,
    num_probes: int,
    warmup: int,
    iterations: int,
    timeout_sec: int,
) -> Dict[str, object]:
    proc_ow2 = run_once(
        bench_bin, case_path, "ow2", tile, num_probes, warmup, iterations, timeout_sec
    )
    if proc_ow2.returncode != 0:
        raise RuntimeError(f"OW2 failed for case={case_path}, rc={proc_ow2.returncode}")
    st_ow2 = parse_fqpt_line(proc_ow2.stdout + proc_ow2.stderr)

    proc_ow5 = run_once(
        bench_bin, case_path, "ow5", tile, num_probes, warmup, iterations, timeout_sec
    )
    if proc_ow5.returncode != 0:
        raise RuntimeError(f"OW5 failed for case={case_path}, rc={proc_ow5.returncode}")
    st_ow5 = parse_fqpt_line(proc_ow5.stdout + proc_ow5.stderr)

    ratio = safe_ratio(st_ow5.avg_ms, st_ow2.avg_ms)
    delta_pct = (ratio - 1.0) * 100.0
    unknown_drift = st_ow5.unknown != st_ow2.unknown
    return {
        "case": case_path,
        "ow2_ms": st_ow2.avg_ms,
        "ow5_ms": st_ow5.avg_ms,
        "ratio": ratio,
        "delta_pct": delta_pct,
        "ow2_unknown": st_ow2.unknown,
        "ow5_unknown": st_ow5.unknown,
        "unknown_drift": int(unknown_drift),
        "ow2_line": st_ow2.line,
        "ow5_line": st_ow5.line,
        "raw_ow2": proc_ow2.stdout + proc_ow2.stderr,
        "raw_ow5": proc_ow5.stdout + proc_ow5.stderr,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Evaluate OW5 gate on fixed case sets.")
    parser.add_argument("--bench-bin", default="build/sac_benchmark", help="Path to sac_benchmark.")
    parser.add_argument("--tiles", default="8,4,16", help="OW5 tile sizes to evaluate, comma separated.")
    parser.add_argument("--main-num-probes", type=int, default=256)
    parser.add_argument("--main-warmup", type=int, default=1)
    parser.add_argument("--main-iterations", type=int, default=3)
    parser.add_argument("--stress-num-probes", type=int, default=64)
    parser.add_argument("--stress-warmup", type=int, default=0)
    parser.add_argument("--stress-iterations", type=int, default=1)
    parser.add_argument("--stress-timeout-sec", type=int, default=300)
    parser.add_argument("--main-timeout-sec", type=int, default=300)
    parser.add_argument("--artifacts-dir", default="", help="Output dir; default artifacts/ow5_gate_<timestamp>.")
    args = parser.parse_args()

    bench_bin = pathlib.Path(args.bench_bin)
    if not bench_bin.exists():
        print(f"ERROR: benchmark binary not found: {bench_bin}", file=sys.stderr)
        return 2

    tiles: List[int] = []
    for t in args.tiles.split(","):
        t = t.strip()
        if not t:
            continue
        v = int(t)
        if v not in (4, 8, 16):
            raise ValueError(f"Invalid tile={v}, only 4/8/16 are allowed")
        tiles.append(v)
    if 8 not in tiles:
        tiles.insert(0, 8)

    ts = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    artifacts_dir = pathlib.Path(args.artifacts_dir) if args.artifacts_dir else pathlib.Path("artifacts") / f"ow5_gate_{ts}"
    artifacts_dir.mkdir(parents=True, exist_ok=True)
    raw_log = artifacts_dir / "raw.log"
    main_csv = artifacts_dir / "main.csv"
    stress_csv = artifacts_dir / "stress.csv"
    summary_md = artifacts_dir / "summary.md"

    main_rows_by_tile: Dict[int, List[Dict[str, object]]] = {t: [] for t in tiles}
    stress_rows_by_tile: Dict[int, List[Dict[str, object]]] = {t: [] for t in tiles}

    with raw_log.open("w", encoding="utf-8") as raw:
        for tile in tiles:
            for case in MAIN_CASES:
                row = run_case_pair(
                    bench_bin=bench_bin,
                    case_path=case,
                    tile=tile,
                    num_probes=args.main_num_probes,
                    warmup=args.main_warmup,
                    iterations=args.main_iterations,
                    timeout_sec=args.main_timeout_sec,
                )
                main_rows_by_tile[tile].append(row)
                raw.write(f"===== MAIN TILE={tile} CASE={case} MODE=OW2 =====\n")
                raw.write(str(row["raw_ow2"]))
                raw.write("\n")
                raw.write(f"===== MAIN TILE={tile} CASE={case} MODE=OW5 =====\n")
                raw.write(str(row["raw_ow5"]))
                raw.write("\n")

            for case in STRESS_CASES:
                row = run_case_pair(
                    bench_bin=bench_bin,
                    case_path=case,
                    tile=tile,
                    num_probes=args.stress_num_probes,
                    warmup=args.stress_warmup,
                    iterations=args.stress_iterations,
                    timeout_sec=args.stress_timeout_sec,
                )
                stress_rows_by_tile[tile].append(row)
                raw.write(f"===== STRESS TILE={tile} CASE={case} MODE=OW2 =====\n")
                raw.write(str(row["raw_ow2"]))
                raw.write("\n")
                raw.write(f"===== STRESS TILE={tile} CASE={case} MODE=OW5 =====\n")
                raw.write(str(row["raw_ow5"]))
                raw.write("\n")

    main_fields = [
        "tile",
        "case",
        "ow2_ms",
        "ow5_ms",
        "ratio",
        "delta_pct",
        "ow2_unknown",
        "ow5_unknown",
        "unknown_drift",
        "ow2_line",
        "ow5_line",
    ]
    with main_csv.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=main_fields)
        writer.writeheader()
        for tile in tiles:
            for r in main_rows_by_tile[tile]:
                writer.writerow(
                    {
                        "tile": tile,
                        "case": r["case"],
                        "ow2_ms": f"{float(r['ow2_ms']):.3f}",
                        "ow5_ms": f"{float(r['ow5_ms']):.3f}",
                        "ratio": f"{float(r['ratio']):.6f}",
                        "delta_pct": f"{float(r['delta_pct']):.2f}",
                        "ow2_unknown": r["ow2_unknown"],
                        "ow5_unknown": r["ow5_unknown"],
                        "unknown_drift": r["unknown_drift"],
                        "ow2_line": r["ow2_line"],
                        "ow5_line": r["ow5_line"],
                    }
                )

    stress_fields = [
        "tile",
        "case",
        "ow2_ms",
        "ow5_ms",
        "ratio",
        "delta_pct",
        "ow2_unknown",
        "ow5_unknown",
        "unknown_drift",
    ]
    with stress_csv.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=stress_fields)
        writer.writeheader()
        for tile in tiles:
            for r in stress_rows_by_tile[tile]:
                writer.writerow(
                    {
                        "tile": tile,
                        "case": r["case"],
                        "ow2_ms": f"{float(r['ow2_ms']):.3f}",
                        "ow5_ms": f"{float(r['ow5_ms']):.3f}",
                        "ratio": f"{float(r['ratio']):.6f}",
                        "delta_pct": f"{float(r['delta_pct']):.2f}",
                        "ow2_unknown": r["ow2_unknown"],
                        "ow5_unknown": r["ow5_unknown"],
                        "unknown_drift": r["unknown_drift"],
                    }
                )

    gate_rows = main_rows_by_tile[8]
    gate_ratios = [float(r["ratio"]) for r in gate_rows]
    gate_median = statistics.median(gate_ratios) if gate_ratios else 0.0
    gate_unknown_drift = any(int(r["unknown_drift"]) != 0 for r in gate_rows)
    gate_pass = (gate_median <= 1.0) and (not gate_unknown_drift)

    with summary_md.open("w", encoding="utf-8") as f:
        f.write("# OW5 Gate Summary\n\n")
        f.write(f"- artifacts: `{artifacts_dir}`\n")
        f.write(f"- gate tile: `8`\n")
        f.write(f"- gate median_ratio: `{gate_median:.6f}`\n")
        f.write(f"- gate unknown_drift: `{str(gate_unknown_drift).lower()}`\n")
        f.write(f"- gate result: **{'PASS' if gate_pass else 'FAIL'}**\n\n")

        for tile in tiles:
            rows = main_rows_by_tile[tile]
            ratios = [float(r["ratio"]) for r in rows]
            med = statistics.median(ratios) if ratios else 0.0
            mean = statistics.mean(ratios) if ratios else 0.0
            max_ratio = max(ratios) if ratios else 0.0
            drift = any(int(r["unknown_drift"]) != 0 for r in rows)

            f.write(f"## Main Cases (tile={tile})\n\n")
            f.write(f"- median_ratio: `{med:.6f}`\n")
            f.write(f"- mean_ratio: `{mean:.6f}`\n")
            f.write(f"- max_ratio: `{max_ratio:.6f}`\n")
            f.write(f"- unknown_drift: `{str(drift).lower()}`\n\n")
            f.write("| case | ow2_ms | ow5_ms | ratio | delta% | unknown_drift |\n")
            f.write("|---|---:|---:|---:|---:|---:|\n")
            for r in rows:
                f.write(
                    f"| {r['case']} | {float(r['ow2_ms']):.3f} | {float(r['ow5_ms']):.3f} | "
                    f"{float(r['ratio']):.6f} | {float(r['delta_pct']):.2f} | {int(r['unknown_drift'])} |\n"
                )
            f.write("\n")

        f.write("## Stress Cases (trend only)\n\n")
        for tile in tiles:
            f.write(f"### tile={tile}\n\n")
            f.write("| case | ow2_ms | ow5_ms | ratio | delta% | unknown_drift |\n")
            f.write("|---|---:|---:|---:|---:|---:|\n")
            for r in stress_rows_by_tile[tile]:
                f.write(
                    f"| {r['case']} | {float(r['ow2_ms']):.3f} | {float(r['ow5_ms']):.3f} | "
                    f"{float(r['ratio']):.6f} | {float(r['delta_pct']):.2f} | {int(r['unknown_drift'])} |\n"
                )
            f.write("\n")

    print(f"Artifacts written to: {artifacts_dir}")
    print(f"- {raw_log}")
    print(f"- {main_csv}")
    print(f"- {stress_csv}")
    print(f"- {summary_md}")
    print(f"Gate(tile=8): {'PASS' if gate_pass else 'FAIL'} (median_ratio={gate_median:.6f})")
    return 0 if gate_pass else 1


if __name__ == "__main__":
    sys.exit(main())
