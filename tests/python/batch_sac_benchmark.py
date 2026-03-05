#!/usr/bin/env python3
"""
批量运行 SAC preprocess（sac_benchmark preprocess modes），并导出 CSV。

用途：
- 用于“推理/编码能力”的 preprocess 口径评测（删值/迭代/work/unknown/time）
- 与 e2e tier（batch_test_v2/v3）解耦，避免搜索阶段吞没 preprocess 结论
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Dict
from typing import List

from sac_preprocess_tier_definitions import (
    SAC_PREPROCESS_KNOWN_BAD,
    SAC_PREPROCESS_PERF_SENTINELS,
    SAC_PREPROCESS_REGRESSION,
    SAC_PREPROCESS_STRESS,
    SAC_PREPROCESS_TIER0,
    SAC_PREPROCESS_TIER1,
    SAC_PREPROCESS_TIER2,
)
from select_sac_preprocess_benches import SacScanRecord
from select_sac_preprocess_benches import export_csv_atomic
from select_sac_preprocess_benches import load_csv
from select_sac_preprocess_benches import run_sac_benchmark


PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_BIN = PROJECT_ROOT / "build" / "sac_benchmark"


def get_tier(tier: int) -> List[str]:
    if tier == 0:
        return SAC_PREPROCESS_TIER0
    if tier == 1:
        return SAC_PREPROCESS_TIER1
    if tier == 2:
        return SAC_PREPROCESS_TIER2
    raise ValueError(f"invalid tier: {tier}")

def get_suite(name: str, tier_fallback: int) -> List[str]:
    if not name:
        return get_tier(tier_fallback)

    key = name.strip().lower()
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
    raise ValueError(f"invalid suite: {name}")


def print_summary(records: List[SacScanRecord]) -> None:
    ok = [r for r in records if r.ok]
    err = [r for r in records if not r.ok]

    total_time_ms = sum(r.total_time_ms for r in ok)
    total_del = sum(r.total_deletions for r in ok)
    total_probes = sum(r.total_probes for r in ok)
    total_unknown = sum(r.unknown_probes for r in ok)

    print("\n[summary]")
    print(f"  ok={len(ok)} err={len(err)} total={len(records)}")
    if ok:
        print(f"  total_time_ms={total_time_ms:.1f}")
        print(f"  total_deletions={total_del}")
        print(f"  total_probes={total_probes}")
        print(f"  total_unknown={total_unknown}")
        print(f"  avg_deletions_per_instance={total_del / len(ok):.2f}")
        print(f"  avg_time_ms_per_instance={total_time_ms / len(ok):.1f}")

        top = sorted(ok, key=lambda r: (r.total_deletions, r.p95_iterations, r.max_iterations), reverse=True)[:10]
        print("  top10_by_deletions:")
        for r in top:
            print(
                f"    del={r.total_deletions:4d} probes={r.total_probes:5d} "
                f"iters={r.avg_iterations:.2f}/{r.p95_iterations:2d}/{r.max_iterations:2d} "
                f"unk={r.unknown_probes:4d} t={r.total_time_ms:8.1f}ms {r.path}"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description="Batch run sac_benchmark preprocess modes on SAC preprocess tiers.")
    parser.add_argument("--tier", type=int, default=0, choices=[0, 1, 2])
    parser.add_argument(
        "--suite",
        type=str,
        default="",
        help=(
            "Optional suite name (overrides --tier). "
            "Available: tier0/tier1/tier2/regression/perf/stress/known-bad"
        ),
    )
    parser.add_argument("--bin", type=str, default=str(DEFAULT_BIN), help="Path to sac_benchmark")
    parser.add_argument(
        "--mode",
        type=str,
        default="full_sac",
        help="sac_benchmark mode: full_sac / sac1_preprocess / sac3_preprocess",
    )
    parser.add_argument("--nsac-mask", type=int, default=1, help="Enable NSAC mask (1/0)")
    parser.add_argument("--max-sac-rounds", type=int, default=1, help="Max SAC rounds")
    parser.add_argument("--timeout", type=int, default=10, help="Per instance wall-time timeout (sec)")
    parser.add_argument("--limit", type=int, default=0, help="Limit instances (0=all)")
    parser.add_argument("--csv", type=str, default="", help="Export CSV path (optional)")
    parser.add_argument("--resume", action="store_true", help="Resume from existing --csv (skip ok records)")
    parser.add_argument("--retry-errors", action="store_true", help="When resuming, also re-run error records")
    parser.add_argument(
        "--flush-every",
        type=int,
        default=1,
        help="Flush CSV to disk every N newly finished instances (atomic rewrite).",
    )
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    sac_benchmark_bin = Path(args.bin)
    if not sac_benchmark_bin.exists():
        print(f"ERROR: sac_benchmark not found: {sac_benchmark_bin}")
        return 2

    instances = get_suite(args.suite, args.tier)
    if args.limit and args.limit > 0:
        instances = instances[: args.limit]

    csv_path = Path(args.csv) if args.csv else Path()
    if args.resume and not args.csv:
        print("ERROR: --resume requires --csv to be set")
        return 2

    bench_mode = args.mode.strip()
    nsac_mask = bool(args.nsac_mask)

    def make_key(rec: SacScanRecord) -> str:
        return f"{rec.mode}|nsac={int(rec.nsac_mask)}|r={rec.max_sac_rounds}|{rec.path}"

    existing_by_key: Dict[str, SacScanRecord] = {}
    if args.resume and (PROJECT_ROOT / csv_path).exists():
        prev = load_csv(PROJECT_ROOT / csv_path)
        existing_by_key = {make_key(r): r for r in prev}
        print(f"[resume] loaded {len(prev)} records from {args.csv}")

    newly_finished = 0
    try:
        for i, p in enumerate(instances, 1):
            instance_path = Path(p)
            try:
                key = str(instance_path.resolve().relative_to(PROJECT_ROOT))
            except Exception:
                key = str(instance_path)

            run_key = f"{bench_mode}|nsac={int(nsac_mask)}|r={args.max_sac_rounds}|{key}"
            if args.resume and run_key in existing_by_key:
                prev = existing_by_key[run_key]
                if prev.ok:
                    continue
                if (not prev.ok) and (not args.retry_errors):
                    continue

            rec = run_sac_benchmark(
                sac_benchmark_bin,
                instance_path,
                mode=bench_mode,
                nsac_mask=nsac_mask,
                max_sac_rounds=args.max_sac_rounds,
                instance_timeout_sec=args.timeout,
            )
            existing_by_key[make_key(rec)] = rec
            newly_finished += 1

            if args.csv and args.flush_every > 0 and newly_finished % args.flush_every == 0:
                export_csv_atomic(list(existing_by_key.values()), PROJECT_ROOT / csv_path)

            if args.quiet:
                continue
            tag = "OK" if rec.ok else "ERR"
            print(
                f"[{i:3d}/{len(instances)}] {tag} del={rec.total_deletions:4d} "
                f"probes={rec.total_probes:5d} iters={rec.avg_iterations:5.2f}/{rec.p95_iterations:2d}/{rec.max_iterations:2d} "
                f"unk={rec.unknown_probes:4d} t={rec.total_time_ms:8.1f}ms {rec.path}"
            )
    except KeyboardInterrupt:
        print("\n[interrupt] received, flushing CSV and exiting...")
    finally:
        if args.csv:
            export_csv_atomic(list(existing_by_key.values()), PROJECT_ROOT / csv_path)

    records = list(existing_by_key.values())
    print_summary(records)

    if args.csv:
        print(f"\n[csv] {args.csv}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
