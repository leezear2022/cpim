#!/usr/bin/env python3
"""Batch Metal SAC probe benchmark over CPIM tier suites."""

from __future__ import annotations

import argparse
import csv
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(SCRIPT_DIR))

from tier_definitions import get_tier_by_number  # noqa: E402


METAL_SMOKE = [
    PROJECT_ROOT / "tests/data/bench/queens-4_ext.xml",
    PROJECT_ROOT / "tests/data/bench/test.xml",
    PROJECT_ROOT / "tests/data/bench/queens-12_ext.xml",
    PROJECT_ROOT / "tests/data/metal/gac_inconsistent.xml",
    PROJECT_ROOT / "tests/data/metal/gac_bitwords2.xml",
]


@dataclass
class ScanRow:
    tier: str
    instance_index: int
    input: str
    status: str
    error: str = ""
    error_category: str = ""
    run: str = ""
    device: str = ""
    num_vars: str = ""
    num_constraints: str = ""
    max_dom_size: str = ""
    bit_words: str = ""
    sac_mode: str = ""
    activation_mode: str = ""
    probe_fusion: str = ""
    probe_limit: str = ""
    max_probe_rounds: str = ""
    fusion_rounds: str = ""
    max_sac_batches: str = ""
    outer_queue_budget: str = ""
    gac_solve_ms: str = ""
    gac_iterations: str = ""
    gac_deletions: str = ""
    gac_inconsistent: str = ""
    probes: str = ""
    ok_count: str = ""
    dwo_count: str = ""
    unknown_count: str = ""
    rounds: str = ""
    dispatch_count: str = ""
    command_buffer_count: str = ""
    active_frontier_total: str = ""
    allowed_constraints_count: str = ""
    budget_exceeded: str = ""
    elapsed_ms: str = ""
    dispatch_ms: str = ""
    dispatch_encode_ms: str = ""
    dispatch_wait_ms: str = ""
    dispatch_non_kernel_ms: str = ""
    kernel_ms: str = ""
    probes_per_sec: str = ""
    dispatch_per_probe: str = ""
    command_buffer_per_probe: str = ""
    non_kernel_per_probe: str = ""
    stats_fusion_rounds: str = ""
    fused_rounds_encoded: str = ""
    fused_rounds_wasted: str = ""
    nsacq_batches: str = ""
    nsacq_gac_runs: str = ""
    nsacq_gac_deletions: str = ""
    nsacq_deleted_values: str = ""
    nsacq_raw_dwo_count: str = ""
    nsacq_confirmed_dwo_count: str = ""
    nsacq_rejected_dwo_count: str = ""
    nsacq_raw_dwo_precision: str = ""
    nsacq_queue_push_count: str = ""
    nsacq_queue_pop_count: str = ""
    nsacq_final_queue_size: str = ""
    nsacq_final_domain_values: str = ""
    nsacq_elapsed_ms: str = ""
    nsacq_queue_budget_exceeded: str = ""
    dwo_forensic_checked: str = ""
    dwo_domain_size_popcount_mismatch_count: str = ""
    dwo_rejected_empty_domain_count: str = ""
    dwo_rejected_nonempty_domain_count: str = ""
    dwo_first_rejected_var: str = ""
    dwo_first_rejected_value: str = ""
    dwo_first_rejected_empty_var: str = ""
    dwo_first_rejected_empty_popcount: str = ""
    dwo_first_rejected_empty_domain_size: str = ""
    dwo_first_rejected_status_var: str = ""
    dwo_first_rejected_status_cid: str = ""
    dwo_first_rejected_status_dir: str = ""
    dwo_first_rejected_status_old_size: str = ""
    dwo_first_rejected_status_deletion_count: str = ""
    dwo_first_rejected_status_round: str = ""
    dwo_first_rejected_snapshot_hash: str = ""
    dwo_first_rejected_allowed_hash: str = ""
    gpu_timing_available: str = ""
    verify_checked: str = ""
    verify_mismatches: str = ""
    verified: str = ""


def classify_error(stderr: str, stdout: str, returncode: int) -> str:
    text = f"{stderr}\n{stdout}"
    if returncode == 124 or "TIMEOUT" in text:
        return "timeout"
    if "supports only binary extension" in text:
        return "unsupported_non_binary_extension"
    if "supports only supports semantics" in text:
        return "unsupported_extension_semantics"
    if "unsupported" in text.lower():
        return "unsupported"
    if "Parse/normalize failed" in text:
        return "parse"
    if "mismatch" in text.lower():
        return "verification_mismatch"
    return "error"


def resolve_instances(args: argparse.Namespace) -> tuple[str, List[Path]]:
    if args.instances:
        return "explicit", [Path(p) for p in args.instances]
    if args.suite == "metal-smoke":
        return "metal-smoke", list(METAL_SMOKE)
    return f"tier{args.tier}", [Path(p) for p in get_tier_by_number(args.tier)]


def parse_int_list(value: str) -> List[int]:
    result: List[int] = []
    for part in value.split(","):
        part = part.strip()
        if not part:
            continue
        try:
            parsed = int(part)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(
                f"expected comma-separated integers, got {value!r}"
            ) from exc
        if parsed <= 0:
            raise argparse.ArgumentTypeError("fusion rounds must be positive")
        result.append(parsed)
    if not result:
        raise argparse.ArgumentTypeError("fusion rounds sweep cannot be empty")
    return result


def run_one(args: argparse.Namespace, tier_name: str, index: int, instance: Path) -> List[ScanRow]:
    if not instance.exists():
        return [
            ScanRow(
                tier=tier_name,
                instance_index=index,
                input=str(instance),
                status="MISSING",
                error=f"missing input: {instance}",
                error_category="missing",
                probe_fusion=args.probe_fusion,
                fusion_rounds=str(args.fusion_rounds),
            )
        ]

    with tempfile.NamedTemporaryFile(suffix=".csv", delete=False) as tmp:
        tmp_path = Path(tmp.name)

    cmd = [
        args.bin,
        f"--input={instance}",
        f"--runs={args.runs}",
        f"--warmup={args.warmup}",
        f"--probe_limit={args.probe_limit}",
        f"--sac_mode={args.sac_mode}",
        f"--activation_mode={args.activation_mode}",
        f"--max_probe_rounds={args.max_probe_rounds}",
        f"--probe_fusion={args.probe_fusion}",
        f"--fusion_rounds={args.fusion_rounds}",
        f"--max_sac_batches={args.max_sac_batches}",
        f"--outer_queue_budget={args.outer_queue_budget}",
        f"--verify={'true' if args.verify_probe_limit != 0 else 'false'}",
        f"--verify_probe_limit={max(0, args.verify_probe_limit)}",
        f"--dwo_forensics={'true' if args.dwo_forensics else 'false'}",
        f"--csv={tmp_path}",
    ]
    if args.metallib:
        cmd.append(f"--metallib={args.metallib}")

    start = time.monotonic()
    try:
        proc = subprocess.run(
            cmd,
            cwd=PROJECT_ROOT,
            text=True,
            capture_output=True,
            timeout=args.timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        tmp_path.unlink(missing_ok=True)
        return [
            ScanRow(
                tier=tier_name,
                instance_index=index,
                input=str(instance),
                status="TIMEOUT",
                error=str(exc),
                error_category="timeout",
                probe_fusion=args.probe_fusion,
                fusion_rounds=str(args.fusion_rounds),
            )
        ]

    rows: List[ScanRow] = []
    if proc.returncode != 0:
        tmp_path.unlink(missing_ok=True)
        output_lines = (proc.stderr or proc.stdout).strip().splitlines()
        rows.append(
            ScanRow(
                tier=tier_name,
                instance_index=index,
                input=str(instance),
                status="ERROR",
                error=output_lines[-1] if output_lines else f"returncode={proc.returncode}",
                error_category=classify_error(proc.stderr, proc.stdout, proc.returncode),
                probe_fusion=args.probe_fusion,
                fusion_rounds=str(args.fusion_rounds),
            )
        )
        return rows

    if not tmp_path.exists():
        return [
            ScanRow(
                tier=tier_name,
                instance_index=index,
                input=str(instance),
                status="ERROR",
                error="benchmark did not write CSV",
                error_category="missing_csv",
                probe_fusion=args.probe_fusion,
                fusion_rounds=str(args.fusion_rounds),
            )
        ]

    with tmp_path.open(newline="") as f:
        reader = csv.DictReader(f)
        for raw in reader:
            row = ScanRow(
                tier=tier_name,
                instance_index=index,
                input=raw.get("input", str(instance)),
                status="OK",
            )
            for key in raw:
                if hasattr(row, key):
                    setattr(row, key, raw.get(key, ""))
            rows.append(row)
    tmp_path.unlink(missing_ok=True)

    if not rows:
        rows.append(
            ScanRow(
                tier=tier_name,
                instance_index=index,
                input=str(instance),
                status="ERROR",
                error=f"empty CSV after {time.monotonic() - start:.2f}s",
                error_category="empty_csv",
                probe_fusion=args.probe_fusion,
                fusion_rounds=str(args.fusion_rounds),
            )
        )
    return rows


def write_csv(path: Path, rows: Sequence[ScanRow]) -> None:
    if path.parent:
        path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = list(ScanRow.__dataclass_fields__.keys())
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row.__dict__)


def quantile(values: Sequence[float], percentile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * percentile
    lo = int(rank)
    hi = min(lo + 1, len(ordered) - 1)
    if lo == hi:
        return ordered[lo]
    return ordered[lo] * (hi - rank) + ordered[hi] * (rank - lo)


def metric_summary(values: Sequence[float]) -> str:
    if not values:
        return "avg=0.000000 p50=0.000000 p95=0.000000"
    avg = sum(values) / len(values)
    return (
        f"avg={avg:.6f} p50={quantile(values, 0.50):.6f} "
        f"p95={quantile(values, 0.95):.6f}"
    )


def print_summary(rows: Sequence[ScanRow]) -> None:
    counts: Dict[str, int] = {}
    for row in rows:
        counts[row.status] = counts.get(row.status, 0) + 1
    print("\n[summary]")
    for status in sorted(counts):
        print(f"  {status}: {counts[status]}")
    ok = [row for row in rows if row.status == "OK"]
    if ok:
        probes = sum(float(row.probes or 0.0) for row in ok)
        elapsed = sum(float(row.elapsed_ms or 0.0) for row in ok)
        dispatch_per_probe = [
            float(row.dispatch_per_probe)
            for row in ok
            if row.dispatch_per_probe not in ("", "0")
        ]
        command_buffer_per_probe = [
            float(row.command_buffer_per_probe)
            for row in ok
            if row.command_buffer_per_probe not in ("", "0")
        ]
        non_kernel_per_probe = [
            float(row.non_kernel_per_probe)
            for row in ok
            if row.non_kernel_per_probe not in ("", "0")
        ]
        nsacq_deleted = sum(float(row.nsacq_deleted_values or 0.0) for row in ok)
        nsacq_rejected = sum(float(row.nsacq_rejected_dwo_count or 0.0) for row in ok)
        nsacq_confirmed = sum(float(row.nsacq_confirmed_dwo_count or 0.0) for row in ok)
        forensic_checked = sum(float(row.dwo_forensic_checked or 0.0) for row in ok)
        forensic_mismatch = sum(
            float(row.dwo_domain_size_popcount_mismatch_count or 0.0)
            for row in ok
        )
        fused_wasted = sum(float(row.fused_rounds_wasted or 0.0) for row in ok)
        nsacq_batches = sum(float(row.nsacq_batches or 0.0) for row in ok)
        print(f"  ok_rows={len(ok)} probes={int(probes)} elapsed_ms_sum={elapsed:.3f}")
        if nsacq_batches:
            print(
                f"  nsacq_batches={int(nsacq_batches)} "
                f"nsacq_deleted_values={int(nsacq_deleted)} "
                f"nsacq_confirmed_dwo={int(nsacq_confirmed)} "
                f"nsacq_rejected_dwo={int(nsacq_rejected)}"
            )
        if forensic_checked:
            print(
                f"  dwo_forensic_checked={int(forensic_checked)} "
                f"domain_size_popcount_mismatch={int(forensic_mismatch)}"
            )
        if dispatch_per_probe:
            avg = sum(dispatch_per_probe) / len(dispatch_per_probe)
            print(f"  avg_dispatch_per_probe={avg:.4f}")
        if command_buffer_per_probe:
            avg = sum(command_buffer_per_probe) / len(command_buffer_per_probe)
            print(f"  avg_command_buffer_per_probe={avg:.4f}")
        if non_kernel_per_probe:
            avg = sum(non_kernel_per_probe) / len(non_kernel_per_probe)
            print(f"  avg_non_kernel_per_probe={avg:.6f}")
        if fused_wasted:
            print(f"  fused_rounds_wasted={int(fused_wasted)}")

    groups: Dict[tuple[str, str], List[ScanRow]] = {}
    for row in ok:
        key = (row.probe_fusion or "none", row.fusion_rounds or "0")
        groups.setdefault(key, []).append(row)
    if len(groups) > 1:
        print("\n[fusion summary]")
        for key in sorted(groups):
            group_rows = groups[key]
            probes = sum(float(row.probes or 0.0) for row in group_rows)
            command_buffer_per_probe = [
                float(row.command_buffer_per_probe)
                for row in group_rows
                if row.command_buffer_per_probe not in ("", "0")
            ]
            non_kernel_per_probe = [
                float(row.non_kernel_per_probe)
                for row in group_rows
                if row.non_kernel_per_probe not in ("", "0")
            ]
            dispatch_per_probe = [
                float(row.dispatch_per_probe)
                for row in group_rows
                if row.dispatch_per_probe not in ("", "0")
            ]
            fused_wasted = sum(float(row.fused_rounds_wasted or 0.0) for row in group_rows)
            confirmed = sum(float(row.nsacq_confirmed_dwo_count or 0.0) for row in group_rows)
            rejected = sum(float(row.nsacq_rejected_dwo_count or 0.0) for row in group_rows)
            unknown = sum(float(row.unknown_count or 0.0) for row in group_rows)
            print(
                f"  probe_fusion={key[0]} fusion_rounds={key[1]} "
                f"rows={len(group_rows)} probes={int(probes)} "
                f"dispatch_per_probe[{metric_summary(dispatch_per_probe)}] "
                f"command_buffer_per_probe[{metric_summary(command_buffer_per_probe)}] "
                f"non_kernel_per_probe[{metric_summary(non_kernel_per_probe)}] "
                f"confirmed_dwo={int(confirmed)} rejected_dwo={int(rejected)} "
                f"unknown={int(unknown)} fused_wasted={int(fused_wasted)}"
            )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run Metal SAC batch probe benchmark over tier suites."
    )
    parser.add_argument("--tier", type=int, default=0, choices=[0, 1, 2, 3])
    parser.add_argument(
        "--suite",
        default="tier",
        choices=["tier", "metal-smoke"],
    )
    parser.add_argument("--instances", nargs="*")
    parser.add_argument("--bin", default="build_metal/benchmark_metal_sac")
    parser.add_argument("--metallib", default="")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--probe-limit", type=int, default=0)
    parser.add_argument(
        "--sac-mode",
        default="batch_probe",
        choices=["batch_probe", "nsacq", "sacq_adj", "sacq_full"],
    )
    parser.add_argument("--activation-mode", default="neighbor", choices=["neighbor", "full"])
    parser.add_argument("--max-probe-rounds", type=int, default=10000)
    parser.add_argument("--probe-fusion", default="none", choices=["none", "bounded"])
    parser.add_argument("--fusion-rounds", type=int, default=4)
    parser.add_argument(
        "--fusion-rounds-sweep",
        type=parse_int_list,
        default=[],
        help="Comma-separated fusion_rounds values to run, e.g. 2,4,8.",
    )
    parser.add_argument("--max-sac-batches", type=int, default=10000)
    parser.add_argument("--outer-queue-budget", type=int, default=0)
    parser.add_argument(
        "--verify-probe-limit",
        type=int,
        default=0,
        help="0 disables benchmark verification; positive verifies that many probes.",
    )
    parser.add_argument(
        "--dwo-forensics",
        dest="dwo_forensics",
        action="store_true",
        default=True,
        help="Collect rejected-DWO forensic counters in NSACQ runs.",
    )
    parser.add_argument(
        "--no-dwo-forensics",
        dest="dwo_forensics",
        action="store_false",
        help="Disable rejected-DWO forensic world-domain readback.",
    )
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--csv", required=True)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    tier_name, instances = resolve_instances(args)
    rows: List[ScanRow] = []
    sweep_values = args.fusion_rounds_sweep or [args.fusion_rounds]
    for sweep_index, fusion_rounds in enumerate(sweep_values):
        args.fusion_rounds = fusion_rounds
        for index, instance in enumerate(instances):
            if not args.quiet:
                if len(sweep_values) > 1:
                    print(
                        f"[run] sweep {sweep_index + 1}/{len(sweep_values)} "
                        f"fusion_rounds={fusion_rounds} "
                        f"{index + 1}/{len(instances)} {instance}"
                    )
                else:
                    print(f"[run] {index + 1}/{len(instances)} {instance}")
            rows.extend(run_one(args, tier_name, index, instance))
    write_csv(Path(args.csv), rows)
    print_summary(rows)
    print(f"[write] {args.csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
