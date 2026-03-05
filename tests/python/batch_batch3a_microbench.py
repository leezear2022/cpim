#!/usr/bin/env python3
"""
Batch-3A microbenchmark（P2-2c）：扫参数并导出 CSV（支持断点续跑）。

目标：
- 只测算子吞吐（`./build/sac_benchmark --mode=stage2|batch3a`），不走 preprocess（SAC1/SAC3）也不走搜索。
- 重点用于 P2-2（mapping=2 / kWarpPerWordLaneWorld）的 G/padding 扫描，并**同口径对照 Stage2**。

设计原则：
- 默认行为不改代码：只是批跑并沉淀曲线。
- 可中断可恢复：--resume + --flush-every 原子写 CSV。
"""

from __future__ import annotations

import argparse
import csv
import re
import subprocess
import sys
import time
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

from sac_preprocess_tier_definitions import (
    SAC_PREPROCESS_KNOWN_BAD,
    SAC_PREPROCESS_PERF_SENTINELS,
    SAC_PREPROCESS_REGRESSION,
    SAC_PREPROCESS_STRESS,
    SAC_PREPROCESS_TIER0,
    SAC_PREPROCESS_TIER1,
    SAC_PREPROCESS_TIER2,
)


PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_BIN = PROJECT_ROOT / "build" / "sac_benchmark"

_RE_PROBES = re.compile(r"^\s*Probes:\s*(\d+)\s*$", re.MULTILINE)
_RE_STAGE2 = re.compile(
    r"^\s*(Stage-2)\s+"
    r"(\d+(?:\.\d+)?)\s+ms\s+"
    r"\[(\d+(?:\.\d+)?)\s*-\s*(\d+(?:\.\d+)?)\]\s+"
    r"(\d+)\s+failures\s+"
    r"(\d+(?:\.\d+)?)\s+probes/s\s*$",
    re.MULTILINE,
)
_RE_BATCH3A = re.compile(
    r"^\s*(Batch-3A(?:\s+\(N/A\))?)\s+"
    r"(\d+(?:\.\d+)?)\s+ms\s+"
    r"\[(\d+(?:\.\d+)?)\s*-\s*(\d+(?:\.\d+)?)\]\s+"
    r"(\d+)\s+failures\s+"
    r"(\d+(?:\.\d+)?)\s+probes/s\s*$",
    re.MULTILINE,
)


@dataclass(frozen=True)
class Batch3AMicrobenchRecord:
    path: str
    ok: bool
    error: str
    mode: str  # stage2 | batch3a
    num_probes: int
    warmup: int
    iterations: int
    check_mapping: int
    subwarp_size: int
    worlds_per_block: int
    shmem_padding: int
    avg_time_ms: float
    min_time_ms: float
    max_time_ms: float
    failures: int
    probes_per_sec: float
    stage2_avg_time_ms: float
    speedup_vs_stage2: float
    status: str


def _resolve_instances(suite: str, tier_fallback: int) -> List[str]:
    if not suite:
        suite = f"tier{tier_fallback}"
    key = suite.strip().lower()
    if key in ("tier0", "t0"):
        return SAC_PREPROCESS_TIER0
    if key in ("tier1", "t1"):
        return SAC_PREPROCESS_TIER1
    if key in ("tier2", "t2"):
        return SAC_PREPROCESS_TIER2
    if key in ("regression", "reg", "smoke"):
        return SAC_PREPROCESS_REGRESSION
    if key in ("perf", "performance", "sentinel", "sentinels"):
        return SAC_PREPROCESS_PERF_SENTINELS
    if key in ("stress",):
        return SAC_PREPROCESS_STRESS
    if key in ("known-bad", "known_bad", "bad"):
        return SAC_PREPROCESS_KNOWN_BAD
    raise ValueError(f"invalid suite: {suite}")


def _parse_int_list(csv_list: str) -> List[int]:
    out: List[int] = []
    for s in (csv_list or "").split(","):
        s = s.strip()
        if not s:
            continue
        out.append(int(s))
    return out


def _csv_headers() -> List[str]:
    return [
        "path",
        "ok",
        "error",
        "mode",
        "num_probes",
        "warmup",
        "iterations",
        "check_mapping",
        "subwarp_size",
        "worlds_per_block",
        "shmem_padding",
        "avg_time_ms",
        "min_time_ms",
        "max_time_ms",
        "failures",
        "probes_per_sec",
        "stage2_avg_time_ms",
        "speedup_vs_stage2",
        "status",
    ]


def export_csv_atomic(records: List[Batch3AMicrobenchRecord], csv_path: Path) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = csv_path.with_suffix(csv_path.suffix + ".tmp")
    with tmp.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(_csv_headers())
        for r in records:
            w.writerow(
                [
                    r.path,
                    int(r.ok),
                    r.error,
                    r.mode,
                    r.num_probes,
                    r.warmup,
                    r.iterations,
                    r.check_mapping,
                    r.subwarp_size,
                    r.worlds_per_block,
                    r.shmem_padding,
                    f"{r.avg_time_ms:.6f}",
                    f"{r.min_time_ms:.6f}",
                    f"{r.max_time_ms:.6f}",
                    r.failures,
                    f"{r.probes_per_sec:.6f}",
                    f"{r.stage2_avg_time_ms:.6f}",
                    f"{r.speedup_vs_stage2:.6f}",
                    r.status,
                ]
            )
    tmp.replace(csv_path)


def load_csv(csv_path: Path) -> List[Batch3AMicrobenchRecord]:
    if not csv_path.exists():
        return []
    with csv_path.open("r", newline="") as f:
        r = csv.DictReader(f)
        out: List[Batch3AMicrobenchRecord] = []
        for row in r:
            out.append(
                Batch3AMicrobenchRecord(
                    path=row["path"],
                    ok=bool(int(row["ok"])),
                    error=row.get("error", ""),
                    mode=row.get("mode", "batch3a"),
                    num_probes=int(row.get("num_probes", "0")),
                    warmup=int(row.get("warmup", "0")),
                    iterations=int(row.get("iterations", "0")),
                    check_mapping=int(row.get("check_mapping", "0")),
                    subwarp_size=int(row.get("subwarp_size", "0")),
                    worlds_per_block=int(row.get("worlds_per_block", "0")),
                    shmem_padding=int(row.get("shmem_padding", "0")),
                    avg_time_ms=float(row.get("avg_time_ms", "0")),
                    min_time_ms=float(row.get("min_time_ms", "0")),
                    max_time_ms=float(row.get("max_time_ms", "0")),
                    failures=int(row.get("failures", "0")),
                    probes_per_sec=float(row.get("probes_per_sec", "0")),
                    stage2_avg_time_ms=float(row.get("stage2_avg_time_ms", "0")),
                    speedup_vs_stage2=float(row.get("speedup_vs_stage2", "0")),
                    status=row.get("status", ""),
                )
            )
        return out


def _make_key(r: Batch3AMicrobenchRecord) -> str:
    return (
        f"{r.mode}|p={r.num_probes}|w={r.warmup}|it={r.iterations}"
        f"|m={r.check_mapping}|sw={r.subwarp_size}|G={r.worlds_per_block}"
        f"|pad={r.shmem_padding}|{r.path}"
    )


def _parse_stage2_stdout(stdout: str) -> Tuple[int, float, float, float, int, float, str]:
    m = _RE_PROBES.search(stdout)
    num_probes = int(m.group(1)) if m else 0
    m2 = _RE_STAGE2.search(stdout)
    if not m2:
        raise ValueError("missing Stage-2 result line")
    avg_time_ms = float(m2.group(2))
    min_time_ms = float(m2.group(3))
    max_time_ms = float(m2.group(4))
    failures = int(m2.group(5))
    probes_per_sec = float(m2.group(6))
    return (num_probes, avg_time_ms, min_time_ms, max_time_ms, failures, probes_per_sec, "OK")


def _parse_batch3a_stdout(stdout: str) -> Tuple[int, float, float, float, int, float, str]:
    # Probes 数以程序实际生成的 probe tasks 为准（可能小于请求值）
    m = _RE_PROBES.search(stdout)
    num_probes = int(m.group(1)) if m else 0

    if "Instance not suitable for Batch-3A" in stdout:
        return (num_probes, 0.0, 0.0, 0.0, 0, 0.0, "NOT_SUITABLE")

    m2 = _RE_BATCH3A.search(stdout)
    if not m2:
        raise ValueError("missing Batch-3A result line")

    name = m2.group(1).strip()
    avg_time_ms = float(m2.group(2))
    min_time_ms = float(m2.group(3))
    max_time_ms = float(m2.group(4))
    failures = int(m2.group(5))
    probes_per_sec = float(m2.group(6))
    status = "OK" if name == "Batch-3A" else "N/A"
    return (num_probes, avg_time_ms, min_time_ms, max_time_ms, failures, probes_per_sec, status)


def run_stage2_microbench(
    sac_benchmark_bin: Path,
    instance: Path,
    *,
    num_probes: int,
    warmup: int,
    iterations: int,
    instance_timeout_sec: int,
) -> Batch3AMicrobenchRecord:
    try:
        instance_key = str(instance.resolve().relative_to(PROJECT_ROOT))
    except Exception:
        instance_key = str(instance)

    cmd = [
        str(sac_benchmark_bin),
        f"--input={instance}",
        "--mode=stage2",
        f"--num_probes={num_probes}",
        f"--warmup={warmup}",
        f"--iterations={iterations}",
        "--verbose=false",
    ]

    start = time.time()
    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=instance_timeout_sec,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return Batch3AMicrobenchRecord(
            path=instance_key,
            ok=False,
            error=f"TIMEOUT>{instance_timeout_sec}s",
            mode="stage2",
            num_probes=0,
            warmup=warmup,
            iterations=iterations,
            check_mapping=-1,
            subwarp_size=0,
            worlds_per_block=0,
            shmem_padding=0,
            avg_time_ms=float(instance_timeout_sec) * 1000.0,
            min_time_ms=0.0,
            max_time_ms=0.0,
            failures=0,
            probes_per_sec=0.0,
            stage2_avg_time_ms=0.0,
            speedup_vs_stage2=0.0,
            status="WALL_TIMEOUT",
        )

    elapsed_ms = (time.time() - start) * 1000.0
    stdout = proc.stdout or ""
    if proc.returncode != 0:
        tail = stdout.strip().splitlines()[-1] if stdout.strip() else f"rc={proc.returncode}"
        return Batch3AMicrobenchRecord(
            path=instance_key,
            ok=False,
            error=f"RC={proc.returncode}:{tail[:120]}",
            mode="stage2",
            num_probes=0,
            warmup=warmup,
            iterations=iterations,
            check_mapping=-1,
            subwarp_size=0,
            worlds_per_block=0,
            shmem_padding=0,
            avg_time_ms=elapsed_ms,
            min_time_ms=0.0,
            max_time_ms=0.0,
            failures=0,
            probes_per_sec=0.0,
            stage2_avg_time_ms=0.0,
            speedup_vs_stage2=0.0,
            status="ERROR",
        )

    try:
        (
            probes_actual,
            avg_time_ms,
            min_time_ms,
            max_time_ms,
            failures,
            probes_per_sec,
            status,
        ) = _parse_stage2_stdout(stdout)
    except Exception as e:
        tail = stdout.strip().splitlines()[-1] if stdout.strip() else ""
        return Batch3AMicrobenchRecord(
            path=instance_key,
            ok=False,
            error=f"PARSE_ERROR:{e}:{tail[:120]}",
            mode="stage2",
            num_probes=0,
            warmup=warmup,
            iterations=iterations,
            check_mapping=-1,
            subwarp_size=0,
            worlds_per_block=0,
            shmem_padding=0,
            avg_time_ms=elapsed_ms,
            min_time_ms=0.0,
            max_time_ms=0.0,
            failures=0,
            probes_per_sec=0.0,
            stage2_avg_time_ms=0.0,
            speedup_vs_stage2=0.0,
            status="PARSE_ERROR",
        )

    return Batch3AMicrobenchRecord(
        path=instance_key,
        ok=True,
        error="",
        mode="stage2",
        num_probes=probes_actual,
        warmup=warmup,
        iterations=iterations,
        check_mapping=-1,
        subwarp_size=0,
        worlds_per_block=0,
        shmem_padding=0,
        avg_time_ms=avg_time_ms,
        min_time_ms=min_time_ms,
        max_time_ms=max_time_ms,
        failures=failures,
        probes_per_sec=probes_per_sec,
        stage2_avg_time_ms=avg_time_ms,
        speedup_vs_stage2=1.0,
        status=status,
    )


def run_batch3a_microbench(
    sac_benchmark_bin: Path,
    instance: Path,
    *,
    num_probes: int,
    warmup: int,
    iterations: int,
    check_mapping: int,
    subwarp_size: int,
    worlds_per_block: int,
    shmem_padding: int,
    instance_timeout_sec: int,
    stage2_avg_time_ms: float,
) -> Batch3AMicrobenchRecord:
    try:
        instance_key = str(instance.resolve().relative_to(PROJECT_ROOT))
    except Exception:
        instance_key = str(instance)

    cmd = [
        str(sac_benchmark_bin),
        f"--input={instance}",
        "--mode=batch3a",
        f"--num_probes={num_probes}",
        f"--warmup={warmup}",
        f"--iterations={iterations}",
        f"--batch3a_check_mapping={check_mapping}",
        f"--batch3a_subwarp_size={subwarp_size}",
        f"--batch3a_worlds_per_block={worlds_per_block}",
        f"--batch3a_shmem_padding={shmem_padding}",
        "--verbose=false",
    ]

    start = time.time()
    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=instance_timeout_sec,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return Batch3AMicrobenchRecord(
            path=instance_key,
            ok=False,
            error=f"TIMEOUT>{instance_timeout_sec}s",
            mode="batch3a",
            num_probes=0,
            warmup=warmup,
            iterations=iterations,
            check_mapping=check_mapping,
            subwarp_size=subwarp_size,
            worlds_per_block=worlds_per_block,
            shmem_padding=shmem_padding,
            avg_time_ms=float(instance_timeout_sec) * 1000.0,
            min_time_ms=0.0,
            max_time_ms=0.0,
            failures=0,
            probes_per_sec=0.0,
            stage2_avg_time_ms=stage2_avg_time_ms,
            speedup_vs_stage2=0.0,
            status="WALL_TIMEOUT",
        )

    elapsed_ms = (time.time() - start) * 1000.0
    stdout = proc.stdout or ""
    if proc.returncode != 0:
        tail = stdout.strip().splitlines()[-1] if stdout.strip() else f"rc={proc.returncode}"
        return Batch3AMicrobenchRecord(
            path=instance_key,
            ok=False,
            error=f"RC={proc.returncode}:{tail[:120]}",
            mode="batch3a",
            num_probes=0,
            warmup=warmup,
            iterations=iterations,
            check_mapping=check_mapping,
            subwarp_size=subwarp_size,
            worlds_per_block=worlds_per_block,
            shmem_padding=shmem_padding,
            avg_time_ms=elapsed_ms,
            min_time_ms=0.0,
            max_time_ms=0.0,
            failures=0,
            probes_per_sec=0.0,
            stage2_avg_time_ms=stage2_avg_time_ms,
            speedup_vs_stage2=0.0,
            status="ERROR",
        )

    try:
        (
            probes_actual,
            avg_time_ms,
            min_time_ms,
            max_time_ms,
            failures,
            probes_per_sec,
            status,
        ) = _parse_batch3a_stdout(stdout)
    except Exception as e:
        tail = stdout.strip().splitlines()[-1] if stdout.strip() else ""
        return Batch3AMicrobenchRecord(
            path=instance_key,
            ok=False,
            error=f"PARSE_ERROR:{e}:{tail[:120]}",
            mode="batch3a",
            num_probes=0,
            warmup=warmup,
            iterations=iterations,
            check_mapping=check_mapping,
            subwarp_size=subwarp_size,
            worlds_per_block=worlds_per_block,
            shmem_padding=shmem_padding,
            avg_time_ms=elapsed_ms,
            min_time_ms=0.0,
            max_time_ms=0.0,
            failures=0,
            probes_per_sec=0.0,
            stage2_avg_time_ms=stage2_avg_time_ms,
            speedup_vs_stage2=0.0,
            status="PARSE_ERROR",
        )

    ok = status in ("OK", "NOT_SUITABLE")
    err = "" if ok else "UNKNOWN_STATUS"
    speedup = (stage2_avg_time_ms / avg_time_ms) if (stage2_avg_time_ms > 0 and avg_time_ms > 0) else 0.0
    return Batch3AMicrobenchRecord(
        path=instance_key,
        ok=ok,
        error=err,
        mode="batch3a",
        num_probes=probes_actual,
        warmup=warmup,
        iterations=iterations,
        check_mapping=check_mapping,
        subwarp_size=subwarp_size,
        worlds_per_block=worlds_per_block,
        shmem_padding=shmem_padding,
        avg_time_ms=avg_time_ms,
        min_time_ms=min_time_ms,
        max_time_ms=max_time_ms,
        failures=failures,
        probes_per_sec=probes_per_sec,
        stage2_avg_time_ms=stage2_avg_time_ms,
        speedup_vs_stage2=speedup,
        status=status,
    )


def _print_summary(records: Iterable[Batch3AMicrobenchRecord]) -> None:
    recs = list(records)
    ok = [r for r in recs if r.ok]
    err = [r for r in recs if not r.ok]
    print("\n[summary]")
    print(f"  ok={len(ok)} err={len(err)} total={len(recs)}")
    if ok:
        top = sorted(ok, key=lambda r: r.probes_per_sec, reverse=True)[:10]
        print("  top10_by_probes_per_sec:")
        for r in top:
            print(
                f"    mode={r.mode:6s} pps={r.probes_per_sec:10.1f} t={r.avg_time_ms:8.3f}ms "
                f"m={r.check_mapping:2d} sw={r.subwarp_size:2d} G={r.worlds_per_block:2d} pad={r.shmem_padding} "
                f"{r.path}"
            )

        b_ok = [r for r in ok if r.mode == "batch3a" and r.speedup_vs_stage2 > 0]
        if b_ok:
            top_sp = sorted(b_ok, key=lambda r: r.speedup_vs_stage2, reverse=True)[:10]
            print("  top10_by_speedup_vs_stage2:")
            for r in top_sp:
                print(
                    f"    sp={r.speedup_vs_stage2:6.3f} "
                    f"pps={r.probes_per_sec:10.1f} t={r.avg_time_ms:8.3f}ms "
                    f"m={r.check_mapping:2d} sw={r.subwarp_size:2d} G={r.worlds_per_block:2d} pad={r.shmem_padding} "
                    f"{r.path}"
                )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Batch run sac_benchmark throughput modes (stage2/batch3a) and export CSV."
    )
    parser.add_argument("--tier", type=int, default=0, choices=[0, 1, 2])
    parser.add_argument(
        "--suite",
        type=str,
        default="",
        help="Optional suite name (overrides --tier). Available: tier0/tier1/tier2/regression/perf/stress/known-bad",
    )
    parser.add_argument("--bin", type=str, default=str(DEFAULT_BIN), help="Path to sac_benchmark")
    parser.add_argument("--timeout", type=int, default=60, help="Per instance wall-time timeout (sec)")
    parser.add_argument("--limit", type=int, default=0, help="Limit instances (0=all)")
    parser.add_argument("--csv", type=str, default="", help="Export CSV path (required for --resume)")
    parser.add_argument("--resume", action="store_true", help="Resume from existing --csv (skip ok records)")
    parser.add_argument("--retry-errors", action="store_true", help="When resuming, also re-run error records")
    parser.add_argument("--flush-every", type=int, default=1, help="Flush CSV every N records (atomic rewrite)")

    parser.add_argument("--num-probes", type=int, default=256, help="sac_benchmark --num_probes")
    parser.add_argument("--warmup", type=int, default=1, help="sac_benchmark --warmup")
    parser.add_argument("--iterations", type=int, default=3, help="sac_benchmark --iterations")

    parser.add_argument("--include-stage2", type=int, default=1, choices=[0, 1], help="Run Stage2 baseline (1/0)")
    parser.add_argument(
        "--mappings",
        type=str,
        default="2",
        help="Comma-separated Batch-3A mappings to run: 0/1/2 (default 2)",
    )
    parser.add_argument(
        "--subwarp-sizes",
        type=str,
        default="8",
        help="Comma-separated subwarp sizes for mapping=1: 4/8/16 (default 8)",
    )
    parser.add_argument("--check-mapping", type=int, default=2, help="(deprecated) single mapping, use --mappings")
    parser.add_argument("--subwarp-size", type=int, default=8, help="(deprecated) single subwarp size, use --subwarp-sizes")
    parser.add_argument("--shmem-padding", type=int, default=0, choices=[0, 1], help="shared packing padding (0/1)")
    parser.add_argument(
        "--padding-values",
        type=str,
        default="0",
        help="Comma-separated padding sweep list (0/1). Only applies to mapping=2.",
    )
    parser.add_argument(
        "--g-values",
        type=str,
        default="0,8,16,32",
        help="Comma-separated worlds_per_block (G) sweep list (0=auto)",
    )
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    sac_benchmark_bin = Path(args.bin)
    if not sac_benchmark_bin.exists():
        print(f"ERROR: sac_benchmark not found: {sac_benchmark_bin}", file=sys.stderr)
        return 2

    instances = _resolve_instances(args.suite, args.tier)
    if args.limit and args.limit > 0:
        instances = instances[: args.limit]

    g_values = _parse_int_list(args.g_values)
    if not g_values:
        print("ERROR: --g-values is empty", file=sys.stderr)
        return 2
    pad_values = _parse_int_list(args.padding_values)
    if not pad_values or any(p not in (0, 1) for p in pad_values):
        print("ERROR: --padding-values must be 0/1 list", file=sys.stderr)
        return 2

    mappings = _parse_int_list(args.mappings) or [args.check_mapping]
    mappings = sorted(set(mappings))
    if any(m not in (0, 1, 2) for m in mappings):
        print("ERROR: --mappings must be in {0,1,2}", file=sys.stderr)
        return 2
    # 兼容旧参数：如果用户传了单值 --check-mapping/--subwarp-size，就并入列表。
    if args.check_mapping not in mappings:
        mappings.append(args.check_mapping)
        mappings = sorted(set(mappings))

    subwarp_sizes = _parse_int_list(args.subwarp_sizes) or [args.subwarp_size]
    subwarp_sizes = [s for s in subwarp_sizes if s in (4, 8, 16)]
    if not subwarp_sizes:
        subwarp_sizes = [8]

    csv_path = Path(args.csv) if args.csv else Path()
    if args.resume and not args.csv:
        print("ERROR: --resume requires --csv to be set", file=sys.stderr)
        return 2

    existing_by_key: Dict[str, Batch3AMicrobenchRecord] = {}
    if args.resume and (PROJECT_ROOT / csv_path).exists():
        prev = load_csv(PROJECT_ROOT / csv_path)
        existing_by_key = {_make_key(r): r for r in prev}
        print(f"[resume] loaded {len(prev)} records from {args.csv}")

    newly_finished = 0
    per_instance = 0
    if args.include_stage2:
        per_instance += 1
    if 0 in mappings:
        per_instance += 1
    if 1 in mappings:
        per_instance += len(subwarp_sizes)
    if 2 in mappings:
        per_instance += len(g_values) * len(pad_values)
    total_jobs = len(instances) * per_instance
    job_index = 0

    try:
        for p in instances:
            instance_path = Path(p)

            # --------------------------------------------------------------
            # Stage2 baseline（同口径对照）
            # --------------------------------------------------------------
            stage2_avg = 0.0
            if args.include_stage2:
                job_index += 1
                stage2_key_rec = Batch3AMicrobenchRecord(
                    path=str(instance_path),
                    ok=True,
                    error="",
                    mode="stage2",
                    num_probes=args.num_probes,
                    warmup=args.warmup,
                    iterations=args.iterations,
                    check_mapping=-1,
                    subwarp_size=0,
                    worlds_per_block=0,
                    shmem_padding=0,
                    avg_time_ms=0.0,
                    min_time_ms=0.0,
                    max_time_ms=0.0,
                    failures=0,
                    probes_per_sec=0.0,
                    stage2_avg_time_ms=0.0,
                    speedup_vs_stage2=0.0,
                    status="",
                )
                key = _make_key(stage2_key_rec)
                need_run = True
                if args.resume and key in existing_by_key:
                    prev = existing_by_key[key]
                    if prev.ok or (not args.retry_errors):
                        need_run = False
                        if prev.ok:
                            stage2_avg = prev.avg_time_ms
                if need_run:
                    rec = run_stage2_microbench(
                        sac_benchmark_bin,
                        instance_path,
                        num_probes=args.num_probes,
                        warmup=args.warmup,
                        iterations=args.iterations,
                        instance_timeout_sec=args.timeout,
                    )
                    existing_by_key[_make_key(rec)] = rec
                    newly_finished += 1
                    if rec.ok:
                        stage2_avg = rec.avg_time_ms
                    if args.csv and args.flush_every > 0 and newly_finished % args.flush_every == 0:
                        export_csv_atomic(list(existing_by_key.values()), PROJECT_ROOT / csv_path)
                    if not args.quiet:
                        tag = "OK" if rec.ok else "ERR"
                        print(
                            f"[{job_index:4d}/{total_jobs}] {tag} "
                            f"mode=stage2 pps={rec.probes_per_sec:10.1f} t={rec.avg_time_ms:8.3f}ms {rec.path}"
                        )

            # --------------------------------------------------------------
            # Batch-3A mappings
            # --------------------------------------------------------------
            def run_one_batch3a(
                *,
                mapping: int,
                subwarp_size: int,
                g: int,
                pad: int,
                worlds_per_block: int,
            ) -> None:
                nonlocal newly_finished, job_index
                job_index += 1
                rec_key = Batch3AMicrobenchRecord(
                    path=str(instance_path),
                    ok=True,
                    error="",
                    mode="batch3a",
                    num_probes=args.num_probes,
                    warmup=args.warmup,
                    iterations=args.iterations,
                    check_mapping=mapping,
                    subwarp_size=subwarp_size,
                    worlds_per_block=worlds_per_block,
                    shmem_padding=pad,
                    avg_time_ms=0.0,
                    min_time_ms=0.0,
                    max_time_ms=0.0,
                    failures=0,
                    probes_per_sec=0.0,
                    stage2_avg_time_ms=stage2_avg,
                    speedup_vs_stage2=0.0,
                    status="",
                )
                key = _make_key(rec_key)
                if args.resume and key in existing_by_key:
                    prev = existing_by_key[key]
                    if prev.ok:
                        return
                    if (not prev.ok) and (not args.retry_errors):
                        return

                rec = run_batch3a_microbench(
                    sac_benchmark_bin,
                    instance_path,
                    num_probes=args.num_probes,
                    warmup=args.warmup,
                    iterations=args.iterations,
                    check_mapping=mapping,
                    subwarp_size=subwarp_size,
                    worlds_per_block=g,
                    shmem_padding=pad,
                    instance_timeout_sec=args.timeout,
                    stage2_avg_time_ms=stage2_avg,
                )
                # 记录“实际 worlds_per_block”（避免 mapping!=2 时误导）
                rec = replace(rec, worlds_per_block=worlds_per_block)
                existing_by_key[_make_key(rec)] = rec
                newly_finished += 1

                if args.csv and args.flush_every > 0 and newly_finished % args.flush_every == 0:
                    export_csv_atomic(list(existing_by_key.values()), PROJECT_ROOT / csv_path)

                if args.quiet:
                    return
                tag = "OK" if rec.ok else "ERR"
                print(
                    f"[{job_index:4d}/{total_jobs}] {tag} "
                    f"mode=batch3a sp={rec.speedup_vs_stage2:6.3f} "
                    f"pps={rec.probes_per_sec:10.1f} t={rec.avg_time_ms:8.3f}ms "
                    f"m={rec.check_mapping} sw={rec.subwarp_size:2d} G={rec.worlds_per_block:2d} pad={rec.shmem_padding} "
                    f"{rec.path}"
                )

            if 0 in mappings:
                # mapping=0: warp-per-world（G 固定为 4）
                run_one_batch3a(mapping=0, subwarp_size=0, g=0, pad=0, worlds_per_block=4)

            if 1 in mappings:
                # mapping=1: subwarp-per-world（G=block_size/subwarp_size，block_size=128）
                for sw in subwarp_sizes:
                    run_one_batch3a(mapping=1, subwarp_size=sw, g=0, pad=0, worlds_per_block=128 // sw)

            if 2 in mappings:
                for pad in pad_values:
                    for g in g_values:
                        # mapping=2: worlds_per_block 取请求值（0=auto），记录也用该值
                        run_one_batch3a(mapping=2, subwarp_size=8, g=g, pad=pad, worlds_per_block=g)
    except KeyboardInterrupt:
        print("\n[interrupt] received, flushing CSV and exiting...")
    finally:
        if args.csv:
            export_csv_atomic(list(existing_by_key.values()), PROJECT_ROOT / csv_path)

    records = list(existing_by_key.values())
    _print_summary(records)
    if args.csv:
        print(f"\n[csv] {args.csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
