#!/usr/bin/env python3
"""
数据驱动筛选 “SAC preprocess（sac_benchmark preprocess modes）有效” 的基准集合。

目标：
- 从 benchmarks/ 下扫描可解析的 *_ext.xml / *-ext.xml
- 运行 ./build/sac_benchmark 的 preprocess 模式获取：
  deletions / probes / iters / unknown / time_ms 等
- 输出：
  1) 原始扫描 CSV（用于分析/复现）
  2) 自动挑选的 tier0/tier1/tier2 名单（面向 preprocess 评测）

注意：
- 这里的 “TIMEOUT” 指 max_sac_rounds 用尽导致未收敛，并非 wall-time 超时。
- wall-time 超时由本脚本的 subprocess timeout 控制（instance_timeout_sec）。
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import random
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_BENCH_ROOT = PROJECT_ROOT / "benchmarks"
DEFAULT_BIN = PROJECT_ROOT / "build" / "sac_benchmark"


@dataclasses.dataclass(frozen=True)
class SacScanRecord:
    path: str
    ok: bool
    error: str

    mode: str
    nsac_mask: bool
    max_sac_rounds: int

    total_time_ms: float
    total_probes: int
    total_deletions: int
    avg_iterations: float
    p95_iterations: int
    max_iterations: int
    unknown_probes: int
    status: str


_RE_TOTAL_TIME = re.compile(r"Total time:\s+([0-9.]+)\s+ms")
_RE_TOTAL_PROBES = re.compile(r"Total probes:\s+(\d+)")
_RE_TOTAL_DELETIONS = re.compile(r"Total deletions:\s+(\d+)")
_RE_AVG_ITERS = re.compile(r"Avg iterations:\s+([0-9.]+)")
_RE_P95_ITERS = re.compile(r"P95 iterations:\s+(\d+)")
_RE_MAX_ITERS = re.compile(r"Max iterations:\s+(\d+)")
_RE_UNKNOWN = re.compile(r"Unknown probes:\s+(\d+)\s+\(")
_RE_STATUS = re.compile(r"Status:\s+(.+)")


def _discover_candidates(bench_root: Path) -> List[Path]:
    candidates: List[Path] = []
    for p in bench_root.rglob("*.xml"):
        if not p.is_file():
            continue
        name = p.name
        if not (name.endswith("_ext.xml") or name.endswith("-ext.xml")):
            continue
        # 经验排除：已知多为 predicates/WCSP，且当前 pipeline 不以这些为目标。
        if "_wcsp" in name or "pred" in name:
            continue
        candidates.append(p)
    return sorted(candidates)


def _parse_sac_benchmark_stdout(stdout: str) -> Tuple[float, int, int, float, int, int, int, str]:
    def must(pattern: re.Pattern[str], label: str) -> str:
        m = pattern.search(stdout)
        if not m:
            raise ValueError(f"missing field: {label}")
        return m.group(1).strip()

    total_time_ms = float(must(_RE_TOTAL_TIME, "total_time_ms"))
    total_probes = int(must(_RE_TOTAL_PROBES, "total_probes"))
    total_deletions = int(must(_RE_TOTAL_DELETIONS, "total_deletions"))
    avg_iterations = float(must(_RE_AVG_ITERS, "avg_iterations"))
    p95_iterations = int(must(_RE_P95_ITERS, "p95_iterations"))
    max_iterations = int(must(_RE_MAX_ITERS, "max_iterations"))
    unknown_probes = int(must(_RE_UNKNOWN, "unknown_probes"))
    status = must(_RE_STATUS, "status")
    return (
        total_time_ms,
        total_probes,
        total_deletions,
        avg_iterations,
        p95_iterations,
        max_iterations,
        unknown_probes,
        status,
    )


def run_sac_benchmark(
    sac_benchmark_bin: Path,
    instance: Path,
    *,
    mode: str = "full_sac",
    nsac_mask: bool,
    max_sac_rounds: int,
    instance_timeout_sec: int,
) -> SacScanRecord:
    # 统一用“工程根目录相对路径”存储，保证可复现/可移植
    try:
        instance_key = str(instance.resolve().relative_to(PROJECT_ROOT))
    except Exception:
        instance_key = str(instance)

    cmd = [
        str(sac_benchmark_bin),
        f"--input={instance}",
        f"--mode={mode}",
        f"--max_sac_rounds={max_sac_rounds}",
        f"--nsac_mask={'true' if nsac_mask else 'false'}",
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
        return SacScanRecord(
            path=instance_key,
            ok=False,
            error=f"TIMEOUT>{instance_timeout_sec}s",
            mode=mode,
            nsac_mask=nsac_mask,
            max_sac_rounds=max_sac_rounds,
            total_time_ms=float(instance_timeout_sec) * 1000.0,
            total_probes=0,
            total_deletions=0,
            avg_iterations=0.0,
            p95_iterations=0,
            max_iterations=0,
            unknown_probes=0,
            status="WALL_TIMEOUT",
        )

    elapsed_ms = (time.time() - start) * 1000.0
    stdout = proc.stdout or ""
    if proc.returncode != 0:
        err = stdout.strip().splitlines()[-1] if stdout.strip() else f"rc={proc.returncode}"
        return SacScanRecord(
            path=instance_key,
            ok=False,
            error=f"RC={proc.returncode}:{err[:120]}",
            mode=mode,
            nsac_mask=nsac_mask,
            max_sac_rounds=max_sac_rounds,
            total_time_ms=elapsed_ms,
            total_probes=0,
            total_deletions=0,
            avg_iterations=0.0,
            p95_iterations=0,
            max_iterations=0,
            unknown_probes=0,
            status="ERROR",
        )

    try:
        (
            total_time_ms,
            total_probes,
            total_deletions,
            avg_iterations,
            p95_iterations,
            max_iterations,
            unknown_probes,
            status,
        ) = _parse_sac_benchmark_stdout(stdout)
    except Exception as e:
        tail = stdout.strip().splitlines()[-1] if stdout.strip() else ""
        return SacScanRecord(
            path=instance_key,
            ok=False,
            error=f"PARSE_ERROR:{e}:{tail[:120]}",
            mode=mode,
            nsac_mask=nsac_mask,
            max_sac_rounds=max_sac_rounds,
            total_time_ms=elapsed_ms,
            total_probes=0,
            total_deletions=0,
            avg_iterations=0.0,
            p95_iterations=0,
            max_iterations=0,
            unknown_probes=0,
            status="PARSE_ERROR",
        )

    # total_time_ms 来自程序本身的统计；elapsed_ms 用于兜底诊断。
    _ = elapsed_ms
    return SacScanRecord(
        path=instance_key,
        ok=True,
        error="",
        mode=mode,
        nsac_mask=nsac_mask,
        max_sac_rounds=max_sac_rounds,
        total_time_ms=total_time_ms,
        total_probes=total_probes,
        total_deletions=total_deletions,
        avg_iterations=avg_iterations,
        p95_iterations=p95_iterations,
        max_iterations=max_iterations,
        unknown_probes=unknown_probes,
        status=status,
    )


def export_csv(records: List[SacScanRecord], csv_path: Path) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(
            [
                "path",
                "ok",
                "error",
                "mode",
                "nsac_mask",
                "max_sac_rounds",
                "total_time_ms",
                "total_probes",
                "total_deletions",
                "avg_iterations",
                "p95_iterations",
                "max_iterations",
                "unknown_probes",
                "status",
            ]
        )
        for r in records:
            w.writerow(
                [
                    r.path,
                    int(r.ok),
                    r.error,
                    r.mode,
                    int(r.nsac_mask),
                    r.max_sac_rounds,
                    f"{r.total_time_ms:.3f}",
                    r.total_probes,
                    r.total_deletions,
                    f"{r.avg_iterations:.6f}",
                    r.p95_iterations,
                    r.max_iterations,
                    r.unknown_probes,
                    r.status,
                ]
            )


def export_csv_atomic(records: List[SacScanRecord], csv_path: Path) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = csv_path.with_suffix(csv_path.suffix + ".tmp")
    export_csv(records, tmp)
    tmp.replace(csv_path)


def _select_tiers(
    records: List[SacScanRecord],
    *,
    tier0_size: int,
    tier1_size: int,
    tier2_size: int,
    tier0_max_ms: float,
    tier1_max_ms: float,
) -> Tuple[List[str], List[str], List[str]]:
    ok = [r for r in records if r.ok]

    # 先优先 “删值>0”，其次 max_iters/avg_iters，最后 probes（更大更能压出吞吐差异）
    def key(r: SacScanRecord) -> Tuple[int, int, float, int, float]:
        return (
            1 if r.total_deletions > 0 else 0,
            r.total_deletions,
            r.avg_iterations,
            r.max_iterations,
            r.total_probes,
        )

    ranked = sorted(ok, key=key, reverse=True)

    def pick(limit: int, max_ms: Optional[float], used: set) -> List[str]:
        out: List[str] = []
        for r in ranked:
            if r.path in used:
                continue
            if max_ms is not None and r.total_time_ms > max_ms:
                continue
            out.append(r.path)
            used.add(r.path)
            if len(out) >= limit:
                break
        return out

    used: set = set()
    tier0 = pick(tier0_size, tier0_max_ms, used)
    tier1 = tier0 + pick(max(0, tier1_size - len(tier0)), tier1_max_ms, used)
    tier2 = tier1 + pick(max(0, tier2_size - len(tier1)), None, used)
    return tier0, tier1, tier2


def write_python_tiers(
    tier_py_path: Path, tier0: List[str], tier1: List[str], tier2: List[str]
) -> None:
    tier_py_path.parent.mkdir(parents=True, exist_ok=True)

    def fmt_list(items: List[str]) -> str:
        return "\n".join([f"    _resolve_path({item!r})," for item in items])

    content = f"""#!/usr/bin/env python3
\"\"\"
SAC preprocess 评测集（由 select_sac_preprocess_benches.py 数据驱动生成）。

说明：
- 这些 tier 主要用于跑 `./build/sac_benchmark --mode=full_sac`（preprocess 口径）
- 不追求覆盖所有类型；优先挑选 “删值多/传播深/可复现” 的实例用于展示推理能力与吞吐
\"\"\"

from typing import List

from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent

def _resolve_path(path: str) -> str:
    return str(PROJECT_ROOT / path)

SAC_PREPROCESS_TIER0: List[str] = [
{fmt_list(tier0)}
]

SAC_PREPROCESS_TIER1: List[str] = [
{fmt_list(tier1)}
]

SAC_PREPROCESS_TIER2: List[str] = [
{fmt_list(tier2)}
]
"""
    tier_py_path.write_text(content, encoding="utf-8")


def load_csv(csv_path: Path) -> List[SacScanRecord]:
    with csv_path.open("r", newline="", encoding="utf-8") as f:
        r = csv.DictReader(f)
        out: List[SacScanRecord] = []
        for row in r:
            path = row["path"]
            try:
                p = Path(path)
                if p.is_absolute():
                    path = str(p.resolve().relative_to(PROJECT_ROOT))
            except Exception:
                pass
            out.append(
                SacScanRecord(
                    path=path,
                    ok=row["ok"] == "1",
                    error=row["error"],
                    mode=row.get("mode", "full_sac"),
                    nsac_mask=row["nsac_mask"] == "1",
                    max_sac_rounds=int(row["max_sac_rounds"]),
                    total_time_ms=float(row["total_time_ms"]),
                    total_probes=int(row["total_probes"]),
                    total_deletions=int(row["total_deletions"]),
                    avg_iterations=float(row["avg_iterations"]),
                    p95_iterations=int(row.get("p95_iterations", "0") or 0),
                    max_iterations=int(row["max_iterations"]),
                    unknown_probes=int(row["unknown_probes"]),
                    status=row["status"],
                )
            )
        return out


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Scan benchmarks and select SAC preprocess tiers (based on sac_benchmark preprocess stats)."
    )
    parser.add_argument("--bench-root", type=str, default=str(DEFAULT_BENCH_ROOT), help="Bench root directory")
    parser.add_argument("--bin", type=str, default=str(DEFAULT_BIN), help="Path to sac_benchmark binary")
    parser.add_argument("--nsac-mask", type=int, default=1, help="Use NSAC mask (1/0)")
    parser.add_argument("--max-sac-rounds", type=int, default=1, help="Max SAC rounds (for fast scan)")
    parser.add_argument("--instance-timeout", type=int, default=5, help="Per instance wall-time timeout (seconds)")
    parser.add_argument("--limit", type=int, default=0, help="Limit number of candidates to scan (0=all)")
    parser.add_argument("--csv", type=str, default="out/sac_preprocess_scan.csv", help="Output CSV path")
    parser.add_argument(
        "--from-csv",
        type=str,
        default="",
        help="Load existing scan CSV and only run selection (skip scanning)",
    )
    parser.add_argument(
        "--resume",
        action="store_true",
        help="Resume from --csv if it exists (skip already scanned ok records)",
    )
    parser.add_argument(
        "--retry-errors",
        action="store_true",
        help="When resuming, also re-run previous error records",
    )
    parser.add_argument(
        "--flush-every",
        type=int,
        default=20,
        help="Flush CSV to disk every N newly scanned records (atomic rewrite).",
    )
    parser.add_argument(
        "--shuffle",
        action="store_true",
        help="Shuffle candidates before scanning (useful with --limit for sampling).",
    )
    parser.add_argument("--seed", type=int, default=0, help="Random seed for --shuffle")
    parser.add_argument(
        "--include",
        action="append",
        default=[],
        help="Only include paths containing this substring (repeatable).",
    )
    parser.add_argument(
        "--exclude",
        action="append",
        default=[],
        help="Exclude paths containing this substring (repeatable).",
    )
    parser.add_argument(
        "--write-tiers",
        type=str,
        default="tests/python/sac_preprocess_tier_definitions.py",
        help="Write selected tiers to this python file (empty to disable)",
    )
    parser.add_argument("--tier0-size", type=int, default=12)
    parser.add_argument("--tier1-size", type=int, default=39)
    parser.add_argument("--tier2-size", type=int, default=79)
    parser.add_argument("--tier0-max-ms", type=float, default=1500.0, help="Tier0 max time per instance (ms)")
    parser.add_argument("--tier1-max-ms", type=float, default=8000.0, help="Tier1 max time per instance (ms)")
    args = parser.parse_args()

    sac_benchmark_bin = Path(args.bin)
    if args.from_csv:
        records = load_csv(PROJECT_ROOT / args.from_csv)
        print(f"[load] records={len(records)} from {args.from_csv}")
    else:
        if not sac_benchmark_bin.exists():
            print(f"ERROR: sac_benchmark not found: {sac_benchmark_bin}", file=sys.stderr)
            return 2

        bench_root = Path(args.bench_root)
        if not bench_root.exists():
            print(f"ERROR: bench root not found: {bench_root}", file=sys.stderr)
            return 2

        candidates = _discover_candidates(bench_root)
        include_filters = [s for s in args.include if s]
        exclude_filters = [s for s in args.exclude if s]
        if include_filters:
            candidates = [p for p in candidates if any(s in str(p) for s in include_filters)]
        if exclude_filters:
            candidates = [p for p in candidates if not any(s in str(p) for s in exclude_filters)]

        if args.shuffle:
            rng = random.Random(args.seed)
            rng.shuffle(candidates)

        if args.limit and args.limit > 0:
            candidates = candidates[: args.limit]

        nsac_mask = bool(args.nsac_mask)
        csv_path = PROJECT_ROOT / args.csv

        existing_by_path: Dict[str, SacScanRecord] = {}
        if args.resume and csv_path.exists():
            prev = load_csv(csv_path)
            existing_by_path = {r.path: r for r in prev}
            print(f"[resume] loaded {len(prev)} records from {args.csv}")

        print(
            f"[scan] candidates={len(candidates)} nsac_mask={nsac_mask} rounds={args.max_sac_rounds} "
            f"timeout={args.instance_timeout}s resume={args.resume} retry_errors={args.retry_errors} "
            f"flush_every={args.flush_every} shuffle={args.shuffle}"
        )

        newly_scanned = 0
        try:
            for i, p in enumerate(candidates, 1):
                try:
                    candidate_key = str(p.resolve().relative_to(PROJECT_ROOT))
                except Exception:
                    candidate_key = str(p)

                if candidate_key in existing_by_path:
                    prev = existing_by_path[candidate_key]
                    if prev.ok and args.resume:
                        continue
                    if (not prev.ok) and args.resume and (not args.retry_errors):
                        continue

                rec = run_sac_benchmark(
                    sac_benchmark_bin,
                    p,
                    nsac_mask=nsac_mask,
                    max_sac_rounds=args.max_sac_rounds,
                    instance_timeout_sec=args.instance_timeout,
                )
                existing_by_path[rec.path] = rec
                newly_scanned += 1

                if args.flush_every > 0 and newly_scanned % args.flush_every == 0:
                    export_csv_atomic(list(existing_by_path.values()), csv_path)

                if i % 20 == 0 or not rec.ok:
                    tag = "OK" if rec.ok else "ERR"
                    print(
                        f"[{i:4d}/{len(candidates)}] {tag} del={rec.total_deletions} "
                        f"probes={rec.total_probes} iters={rec.avg_iterations:.2f} "
                        f"t={rec.total_time_ms:.1f}ms {p}"
                    )
        except KeyboardInterrupt:
            print("\n[interrupt] received, flushing CSV and exiting...")
        finally:
            export_csv_atomic(list(existing_by_path.values()), csv_path)

        records = list(existing_by_path.values())

    tier0, tier1, tier2 = _select_tiers(
        records,
        tier0_size=args.tier0_size,
        tier1_size=args.tier1_size,
        tier2_size=args.tier2_size,
        tier0_max_ms=args.tier0_max_ms,
        tier1_max_ms=args.tier1_max_ms,
    )

    print("\n[select] tier0:")
    for p in tier0:
        print(f"  {p}")
    print("\n[select] tier1:")
    for p in tier1:
        print(f"  {p}")
    print("\n[select] tier2:")
    for p in tier2:
        print(f"  {p}")

    if args.write_tiers:
        write_python_tiers(PROJECT_ROOT / args.write_tiers, tier0, tier1, tier2)
        print(f"\n[write] {args.write_tiers}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
