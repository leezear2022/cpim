#!/usr/bin/env python3
"""Batch Metal GAC ablation over CPIM tier suites.

Runs benchmark_metal_gac across readonly storage and frontier modes.  The script
reuses tests/python/tier_definitions.py, but treats missing or unsupported
instances as row-level statuses so a larger tier scan can keep going.
"""

from __future__ import annotations

import argparse
import csv
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(SCRIPT_DIR))

from tier_definitions import get_tier_by_number  # noqa: E402


Mode = Tuple[str, str]

MODE_PRESETS: Dict[str, List[Mode]] = {
    "baseline": [("shared", "flags")],
    "storage": [("shared", "flags"), ("private", "flags")],
    "frontier": [("shared", "flags"), ("shared", "compact"), ("shared", "worklist")],
    "cta": [("shared", "cta_worklist")],
    "bulk_sync": [("shared", "bulk_sync_mask")],
    "bucket_policy": [("shared", "flags")],
    "all": [
        ("shared", "flags"),
        ("private", "flags"),
        ("shared", "compact"),
        ("private", "compact"),
        ("shared", "worklist"),
        ("private", "worklist"),
    ],
    "auto": [("shared", "auto")],
}

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
    runner_mode: str
    readonly_storage: str
    frontier_mode: str
    kernel_variant: str
    bitsup_layout: str
    reset_mode: str
    cta_owner_mode: str
    cta_queue_mode: str
    cta_handoff_mode: str
    cta_local_round_budget: str
    cta_replay_round_budget: str
    cta_dirty_pull_min_degree: str
    status: str
    error: str = ""
    error_category: str = ""
    run: str = ""
    device: str = ""
    num_vars: str = ""
    num_constraints: str = ""
    max_dom_size: str = ""
    bit_words: str = ""
    policy_mode: str = ""
    policy_selected: str = ""
    policy_reason: str = ""
    policy_bucket: str = ""
    effective_frontier_mode: str = ""
    effective_kernel_variant: str = ""
    effective_bitsup_layout: str = ""
    variant_name: str = ""
    iterations: str = ""
    deletions: str = ""
    dispatch_count: str = ""
    inconsistent: str = ""
    budget_exceeded: str = ""
    elapsed_ms: str = ""
    solve_ms: str = ""
    cpu_timing_enabled: str = ""
    cpu_solve_ms: str = ""
    cpu_iterations: str = ""
    cpu_deletions: str = ""
    cpu_inconsistent: str = ""
    metal_cpu_solve_ratio: str = ""
    metal_faster_than_cpu: str = ""
    setup_ms: str = ""
    prepare_ms: str = ""
    reset_ms: str = ""
    reset_dispatch_ms: str = ""
    dispatch_ms: str = ""
    kernel_ms: str = ""
    dispatch_encode_ms: str = ""
    dispatch_wait_ms: str = ""
    dispatch_non_kernel_ms: str = ""
    active_constraints_total: str = ""
    worklist_push_count: str = ""
    worklist_rounds: str = ""
    worklist_epoch_resets: str = ""
    cta_local_rounds: str = ""
    cta_queue_push_count: str = ""
    cta_cross_push_count: str = ""
    cta_overflow_count: str = ""
    cta_queue_overflow_count: str = ""
    cta_budget_spill_count: str = ""
    cta_seed_overflow_count: str = ""
    cta_budget_replay_rounds: str = ""
    cta_budget_replay_drain_count: str = ""
    cta_budget_replay_spill_count: str = ""
    host_round_count: str = ""
    bulk_mask_proposed_deletion_count: str = ""
    bulk_mask_actual_deletion_count: str = ""
    bulk_mask_changed_word_count: str = ""
    bulk_mask_frontier_push_count: str = ""
    bulk_mask_rounds: str = ""
    dirty_var_count: str = ""
    dirty_pull_scan_count: str = ""
    dirty_pull_hit_count: str = ""
    cross_push_avoided_count: str = ""
    dirty_pull_fallback_push_count: str = ""
    owner_map_build_ms: str = ""
    owner_balance_p95: str = ""
    owner_weight_balance_p95: str = ""
    owner_local_push_count: str = ""
    owner_cross_push_count: str = ""
    seed_owner_nonempty_count: str = ""
    seed_empty_owner_count: str = ""
    seed_max_owner_load: str = ""
    seed_owner_balance_p95: str = ""
    frontier_density_avg: str = ""
    gpu_timing_available: str = ""
    verified: str = ""
    process_ms: str = ""


def rel(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(PROJECT_ROOT))
    except ValueError:
        return str(path)


def existing_input_path(path: Path) -> Path:
    candidate = path if path.is_absolute() else PROJECT_ROOT / path
    if candidate.exists():
        return candidate

    try:
        bench_rel = candidate.resolve().relative_to(PROJECT_ROOT / "benchmarks")
    except ValueError:
        return candidate

    sample_candidate = PROJECT_ROOT / "tests/data/bench" / bench_rel
    if sample_candidate.exists():
        return sample_candidate
    return candidate


def resolve_instances(args: argparse.Namespace) -> Tuple[str, List[Path]]:
    if args.instances:
        return "custom", [Path(item).expanduser() for item in args.instances]
    if args.suite == "metal-smoke":
        return "metal-smoke", list(METAL_SMOKE)
    return f"tier{args.tier}", [Path(item) for item in get_tier_by_number(args.tier)]


def resolve_modes(name: str) -> List[Mode]:
    try:
        return MODE_PRESETS[name]
    except KeyError as exc:
        known = ", ".join(sorted(MODE_PRESETS))
        raise ValueError(f"unknown mode preset '{name}', expected one of: {known}") from exc


def read_csv_rows(csv_path: Path) -> List[Dict[str, str]]:
    with csv_path.open("r", newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def short_error(stdout: str, stderr: str) -> str:
    text = (stdout + "\n" + stderr).strip().replace("\n", " | ")
    return text[:500]


def classify_error(text: str) -> str:
    if not text:
        return ""
    lowered = text.lower()
    if "input file does not exist" in lowered:
        return "missing_input"
    if "Predicate/intension references are not supported yet" in text:
        return "unsupported_predicate_intension"
    if "supports only binary extension constraints" in text:
        return "unsupported_non_binary_extension"
    if "global:allDifferent" in text or "global AllDifferent" in text:
        return "unsupported_global_alldifferent"
    if "Failed to parse range max" in text:
        return "parse_discontiguous_domain"
    if "invalid relation ID" in text or "Invalid relation ID" in text:
        return "parse_invalid_relation_id"
    if "CPU and Metal results differ" in text:
        return "verification_mismatch"
    if "budget" in lowered:
        return "budget_or_iteration_failure"
    if "timeout" in lowered:
        return "timeout"
    if "Metal run failed" in text or "Warmup failed" in text:
        return "metal_runtime_failure"
    if "Parse/normalize failed" in text:
        return "parse_or_normalize_failure"
    if "Device layout build failed" in text:
        return "layout_build_failure"
    return "other"


def run_one(
    bench_bin: Path,
    instance: Path,
    mode: Mode,
    args: argparse.Namespace,
    tier_name: str,
    instance_index: int,
) -> List[ScanRow]:
    readonly_storage, frontier_mode = mode
    input_path = existing_input_path(instance)
    input_label = rel(input_path)
    if not input_path.exists():
        return [
            ScanRow(
                tier=tier_name,
                instance_index=instance_index,
                input=input_label,
                runner_mode=args.runner_mode,
                readonly_storage=readonly_storage,
                frontier_mode=frontier_mode,
                kernel_variant=args.kernel_variant,
                bitsup_layout=args.bitsup_layout,
                reset_mode=args.reset_mode,
                cta_owner_mode=args.cta_owner_mode,
                cta_queue_mode=args.cta_queue_mode,
                cta_handoff_mode=args.cta_handoff_mode,
                cta_local_round_budget=str(args.cta_local_round_budget),
                cta_replay_round_budget=str(args.cta_replay_round_budget),
                cta_dirty_pull_min_degree=str(args.cta_dirty_pull_min_degree),
                policy_mode=args.policy_mode,
                status="MISSING",
                error="input file does not exist",
                error_category="missing_input",
            )
        ]

    with tempfile.NamedTemporaryFile(
        prefix="metal_gac_ablation_", suffix=".csv", delete=False
    ) as tmp:
        tmp_csv = Path(tmp.name)

    cmd = [
        str(bench_bin),
        f"--input={input_path}",
        f"--runs={args.runs}",
        f"--warmup={args.warmup}",
        f"--max_iterations={args.max_iterations}",
        f"--verify={'true' if args.verify else 'false'}",
        f"--runner_mode={args.runner_mode}",
        f"--readonly_storage={readonly_storage}",
        f"--frontier_mode={frontier_mode}",
        f"--kernel_variant={args.kernel_variant}",
        f"--bitsup_layout={args.bitsup_layout}",
        f"--reset_mode={args.reset_mode}",
        f"--cta_owner_mode={args.cta_owner_mode}",
        f"--cta_queue_mode={args.cta_queue_mode}",
        f"--cta_handoff_mode={args.cta_handoff_mode}",
        f"--cta_local_round_budget={args.cta_local_round_budget}",
        f"--cta_replay_round_budget={args.cta_replay_round_budget}",
        f"--cta_dirty_pull_min_degree={args.cta_dirty_pull_min_degree}",
        f"--policy_mode={args.policy_mode}",
        f"--csv={tmp_csv}",
    ]
    if args.cpu_timing:
        cmd.append("--cpu_timing=true")
    if args.cpu_warmup >= 0:
        cmd.append(f"--cpu_warmup={args.cpu_warmup}")
    if args.cpu_runs >= 0:
        cmd.append(f"--cpu_runs={args.cpu_runs}")
    if args.metallib:
        cmd.append(f"--metallib={args.metallib}")
    if args.verbose_kernel:
        cmd.append("--verbose=true")

    start = time.monotonic()
    try:
        result = subprocess.run(
            cmd,
            cwd=PROJECT_ROOT,
            text=True,
            capture_output=True,
            timeout=args.timeout,
        )
        process_ms = (time.monotonic() - start) * 1000.0
    except subprocess.TimeoutExpired as exc:
        tmp_csv.unlink(missing_ok=True)
        error = short_error(exc.stdout or "", exc.stderr or "")
        return [
            ScanRow(
                tier=tier_name,
                instance_index=instance_index,
                input=input_label,
                runner_mode=args.runner_mode,
                readonly_storage=readonly_storage,
                frontier_mode=frontier_mode,
                kernel_variant=args.kernel_variant,
                bitsup_layout=args.bitsup_layout,
                reset_mode=args.reset_mode,
                cta_owner_mode=args.cta_owner_mode,
                cta_queue_mode=args.cta_queue_mode,
                cta_handoff_mode=args.cta_handoff_mode,
                cta_local_round_budget=str(args.cta_local_round_budget),
                cta_replay_round_budget=str(args.cta_replay_round_budget),
                cta_dirty_pull_min_degree=str(args.cta_dirty_pull_min_degree),
                policy_mode=args.policy_mode,
                status="TIMEOUT",
                error=error,
                error_category=classify_error(error) or "timeout",
            )
        ]

    if result.returncode != 0:
        tmp_csv.unlink(missing_ok=True)
        error = short_error(result.stdout, result.stderr)
        return [
            ScanRow(
                tier=tier_name,
                instance_index=instance_index,
                input=input_label,
                runner_mode=args.runner_mode,
                readonly_storage=readonly_storage,
                frontier_mode=frontier_mode,
                kernel_variant=args.kernel_variant,
                bitsup_layout=args.bitsup_layout,
                reset_mode=args.reset_mode,
                cta_owner_mode=args.cta_owner_mode,
                cta_queue_mode=args.cta_queue_mode,
                cta_handoff_mode=args.cta_handoff_mode,
                cta_local_round_budget=str(args.cta_local_round_budget),
                cta_replay_round_budget=str(args.cta_replay_round_budget),
                cta_dirty_pull_min_degree=str(args.cta_dirty_pull_min_degree),
                policy_mode=args.policy_mode,
                status="ERROR",
                error=error,
                error_category=classify_error(error),
                process_ms=f"{process_ms:.3f}",
            )
        ]

    rows = []
    try:
        raw_rows = read_csv_rows(tmp_csv)
    finally:
        tmp_csv.unlink(missing_ok=True)

    if not raw_rows:
        return [
            ScanRow(
                tier=tier_name,
                instance_index=instance_index,
                input=input_label,
                runner_mode=args.runner_mode,
                readonly_storage=readonly_storage,
                frontier_mode=frontier_mode,
                kernel_variant=args.kernel_variant,
                bitsup_layout=args.bitsup_layout,
                reset_mode=args.reset_mode,
                cta_owner_mode=args.cta_owner_mode,
                cta_queue_mode=args.cta_queue_mode,
                cta_handoff_mode=args.cta_handoff_mode,
                cta_local_round_budget=str(args.cta_local_round_budget),
                cta_replay_round_budget=str(args.cta_replay_round_budget),
                cta_dirty_pull_min_degree=str(args.cta_dirty_pull_min_degree),
                policy_mode=args.policy_mode,
                status="ERROR",
                error="benchmark succeeded but wrote no CSV rows",
                error_category="empty_benchmark_csv",
                process_ms=f"{process_ms:.3f}",
            )
        ]

    for raw in raw_rows:
        rows.append(
            ScanRow(
                tier=tier_name,
                instance_index=instance_index,
                input=input_label,
                runner_mode=raw.get("runner_mode", args.runner_mode),
                readonly_storage=readonly_storage,
                frontier_mode=raw.get("frontier_mode", frontier_mode),
                kernel_variant=raw.get("kernel_variant", args.kernel_variant),
                bitsup_layout=raw.get("bitsup_layout", args.bitsup_layout),
                reset_mode=raw.get("reset_mode", args.reset_mode),
                cta_owner_mode=raw.get("cta_owner_mode", args.cta_owner_mode),
                cta_queue_mode=raw.get("cta_queue_mode", args.cta_queue_mode),
                cta_handoff_mode=raw.get(
                    "cta_handoff_mode", args.cta_handoff_mode
                ),
                cta_local_round_budget=raw.get(
                    "cta_local_round_budget", str(args.cta_local_round_budget)
                ),
                cta_replay_round_budget=raw.get(
                    "cta_replay_round_budget", str(args.cta_replay_round_budget)
                ),
                cta_dirty_pull_min_degree=raw.get(
                    "cta_dirty_pull_min_degree",
                    str(args.cta_dirty_pull_min_degree),
                ),
                status="OK",
                run=raw.get("run", ""),
                device=raw.get("device", ""),
                num_vars=raw.get("num_vars", ""),
                num_constraints=raw.get("num_constraints", ""),
                max_dom_size=raw.get("max_dom_size", ""),
                bit_words=raw.get("bit_words", ""),
                policy_mode=raw.get("policy_mode", args.policy_mode),
                policy_selected=raw.get("policy_selected", ""),
                policy_reason=raw.get("policy_reason", ""),
                policy_bucket=raw.get("policy_bucket", ""),
                effective_frontier_mode=raw.get("effective_frontier_mode", ""),
                effective_kernel_variant=raw.get("effective_kernel_variant", ""),
                effective_bitsup_layout=raw.get("effective_bitsup_layout", ""),
                variant_name=raw.get("variant_name", ""),
                iterations=raw.get("iterations", ""),
                deletions=raw.get("deletions", ""),
                dispatch_count=raw.get("dispatch_count", ""),
                inconsistent=raw.get("inconsistent", ""),
                budget_exceeded=raw.get("budget_exceeded", ""),
                elapsed_ms=raw.get("elapsed_ms", ""),
                solve_ms=raw.get("solve_ms", raw.get("elapsed_ms", "")),
                cpu_timing_enabled=raw.get("cpu_timing_enabled", ""),
                cpu_solve_ms=raw.get("cpu_solve_ms", ""),
                cpu_iterations=raw.get("cpu_iterations", ""),
                cpu_deletions=raw.get("cpu_deletions", ""),
                cpu_inconsistent=raw.get("cpu_inconsistent", ""),
                metal_cpu_solve_ratio=raw.get("metal_cpu_solve_ratio", ""),
                metal_faster_than_cpu=raw.get("metal_faster_than_cpu", ""),
                setup_ms=raw.get("setup_ms", ""),
                prepare_ms=raw.get("prepare_ms", ""),
                reset_ms=raw.get("reset_ms", ""),
                reset_dispatch_ms=raw.get("reset_dispatch_ms", ""),
                dispatch_ms=raw.get("dispatch_ms", ""),
                kernel_ms=raw.get("kernel_ms", ""),
                dispatch_encode_ms=raw.get("dispatch_encode_ms", ""),
                dispatch_wait_ms=raw.get("dispatch_wait_ms", ""),
                dispatch_non_kernel_ms=raw.get("dispatch_non_kernel_ms", ""),
                active_constraints_total=raw.get("active_constraints_total", ""),
                worklist_push_count=raw.get("worklist_push_count", ""),
                worklist_rounds=raw.get("worklist_rounds", ""),
                worklist_epoch_resets=raw.get("worklist_epoch_resets", ""),
                cta_local_rounds=raw.get("cta_local_rounds", ""),
                cta_queue_push_count=raw.get("cta_queue_push_count", ""),
                cta_cross_push_count=raw.get("cta_cross_push_count", ""),
                cta_overflow_count=raw.get("cta_overflow_count", ""),
                cta_queue_overflow_count=raw.get("cta_queue_overflow_count", ""),
                cta_budget_spill_count=raw.get("cta_budget_spill_count", ""),
                cta_seed_overflow_count=raw.get("cta_seed_overflow_count", ""),
                cta_budget_replay_rounds=raw.get("cta_budget_replay_rounds", ""),
                cta_budget_replay_drain_count=raw.get(
                    "cta_budget_replay_drain_count", ""
                ),
                cta_budget_replay_spill_count=raw.get(
                    "cta_budget_replay_spill_count", ""
                ),
                host_round_count=raw.get("host_round_count", ""),
                bulk_mask_proposed_deletion_count=raw.get(
                    "bulk_mask_proposed_deletion_count", ""
                ),
                bulk_mask_actual_deletion_count=raw.get(
                    "bulk_mask_actual_deletion_count", ""
                ),
                bulk_mask_changed_word_count=raw.get(
                    "bulk_mask_changed_word_count", ""
                ),
                bulk_mask_frontier_push_count=raw.get(
                    "bulk_mask_frontier_push_count", ""
                ),
                bulk_mask_rounds=raw.get("bulk_mask_rounds", ""),
                dirty_var_count=raw.get("dirty_var_count", ""),
                dirty_pull_scan_count=raw.get("dirty_pull_scan_count", ""),
                dirty_pull_hit_count=raw.get("dirty_pull_hit_count", ""),
                cross_push_avoided_count=raw.get("cross_push_avoided_count", ""),
                dirty_pull_fallback_push_count=raw.get(
                    "dirty_pull_fallback_push_count", ""
                ),
                owner_map_build_ms=raw.get("owner_map_build_ms", ""),
                owner_balance_p95=raw.get("owner_balance_p95", ""),
                owner_weight_balance_p95=raw.get("owner_weight_balance_p95", ""),
                owner_local_push_count=raw.get("owner_local_push_count", ""),
                owner_cross_push_count=raw.get("owner_cross_push_count", ""),
                seed_owner_nonempty_count=raw.get("seed_owner_nonempty_count", ""),
                seed_empty_owner_count=raw.get("seed_empty_owner_count", ""),
                seed_max_owner_load=raw.get("seed_max_owner_load", ""),
                seed_owner_balance_p95=raw.get("seed_owner_balance_p95", ""),
                frontier_density_avg=raw.get("frontier_density_avg", ""),
                gpu_timing_available=raw.get("gpu_timing_available", ""),
                verified=raw.get("verified", ""),
                process_ms=f"{process_ms:.3f}",
            )
        )
    return rows


def write_rows(csv_path: Path, rows: Sequence[ScanRow]) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(ScanRow.__dataclass_fields__))
        writer.writeheader()
        for row in rows:
            writer.writerow(row.__dict__)


def summarize(rows: Sequence[ScanRow]) -> None:
    status_counts: Dict[str, int] = {}
    for row in rows:
        status_counts[row.status] = status_counts.get(row.status, 0) + 1

    print("\n[summary]")
    for status in sorted(status_counts):
        print(f"  {status}: {status_counts[status]}")

    ok_rows = [row for row in rows if row.status == "OK"]
    if ok_rows:
        by_mode: Dict[
            Tuple[str, str, str, str, str, str, str, str, str, str], List[float]
        ] = {}
        for row in ok_rows:
            owner_mode = row.cta_owner_mode if row.frontier_mode == "cta_worklist" else ""
            queue_mode = row.cta_queue_mode if row.frontier_mode == "cta_worklist" else ""
            handoff_mode = (
                row.cta_handoff_mode if row.frontier_mode == "cta_worklist" else ""
            )
            local_budget = (
                row.cta_local_round_budget
                if row.frontier_mode == "cta_worklist"
                else ""
            )
            replay_budget = (
                row.cta_replay_round_budget
                if row.frontier_mode == "cta_worklist"
                else ""
            )
            dirty_min_degree = (
                row.cta_dirty_pull_min_degree
                if row.frontier_mode == "cta_worklist"
                else ""
            )
            key = (
                row.readonly_storage,
                row.frontier_mode,
                row.policy_mode,
                row.policy_selected,
                owner_mode,
                queue_mode,
                handoff_mode,
                local_budget,
                replay_budget,
                dirty_min_degree,
            )
            try:
                by_mode.setdefault(key, []).append(float(row.solve_ms))
            except ValueError:
                pass
        for key, values in sorted(by_mode.items()):
            avg = sum(values) / len(values)
            policy_label = (
                f" policy={key[2]} selected={key[3]}" if key[2] else ""
            )
            owner_label = f" owner={key[4]}" if key[4] else ""
            queue_label = f" queue={key[5]}" if key[5] else ""
            handoff_label = f" handoff={key[6]}" if key[6] else ""
            budget_label = (
                f" local_budget={key[7]} replay_budget={key[8]}"
                if key[7]
                else ""
            )
            dirty_degree_label = (
                f" dirty_min_degree={key[9]}" if key[9] else ""
            )
            print(
                f"  mode={key[0]}+{key[1]}{policy_label}"
                f"{owner_label}{queue_label}"
                f"{handoff_label}{budget_label}{dirty_degree_label} "
                f"rows={len(values)} "
                f"avg_solve_ms={avg:.3f}"
            )
        by_runner: Dict[str, int] = {}
        for row in ok_rows:
            by_runner[row.runner_mode] = by_runner.get(row.runner_mode, 0) + 1
        for runner, count in sorted(by_runner.items()):
            print(f"  runner={runner} ok_rows={count}")
        cpu_ratios = []
        for row in ok_rows:
            try:
                ratio = float(row.metal_cpu_solve_ratio)
            except ValueError:
                continue
            if ratio > 0.0:
                cpu_ratios.append(ratio)
        if cpu_ratios:
            faster = sum(1 for row in ok_rows if row.metal_faster_than_cpu == "true")
            avg_ratio = sum(cpu_ratios) / len(cpu_ratios)
            print(
                f"  metal_vs_cpu rows={len(cpu_ratios)} "
                f"avg_ratio={avg_ratio:.3f} metal_faster={faster}"
            )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run Metal GAC storage/frontier ablation over tier suites."
    )
    parser.add_argument("--tier", type=int, default=0, choices=[0, 1, 2, 3])
    parser.add_argument(
        "--suite",
        default="tier",
        choices=["tier", "metal-smoke"],
        help="Instance suite. 'tier' uses --tier; metal-smoke uses local fixtures.",
    )
    parser.add_argument(
        "--instances",
        nargs="*",
        help="Explicit XML instances. Overrides --suite and --tier.",
    )
    parser.add_argument(
        "--mode-preset",
        default="all",
        choices=sorted(MODE_PRESETS),
        help="Ablation mode set.",
    )
    parser.add_argument("--bin", default="build_metal/benchmark_metal_gac")
    parser.add_argument("--metallib", default="")
    parser.add_argument(
        "--runner-mode",
        default="cold",
        choices=["cold", "prepared"],
        help="Forwarded to benchmark_metal_gac --runner_mode.",
    )
    parser.add_argument(
        "--kernel-variant",
        default="scalar",
        choices=["scalar", "word_parallel", "simdgroup", "auto"],
        help="Forwarded to benchmark_metal_gac --kernel_variant.",
    )
    parser.add_argument(
        "--bitsup-layout",
        default="pair",
        choices=["pair", "directional", "auto"],
        help="Forwarded to benchmark_metal_gac --bitsup_layout.",
    )
    parser.add_argument(
        "--reset-mode",
        default="cpu",
        choices=["cpu", "blit", "auto"],
        help="Forwarded to benchmark_metal_gac --reset_mode.",
    )
    parser.add_argument(
        "--policy-mode",
        default="none",
        choices=["none", "bh_cta_allowlist"],
        help="Forwarded to benchmark_metal_gac --policy_mode.",
    )
    parser.add_argument(
        "--cta-owner-mode",
        default="modulo",
        choices=["modulo", "static_edge_cut", "vebo_weighted"],
        help="Forwarded to benchmark_metal_gac --cta_owner_mode.",
    )
    parser.add_argument(
        "--cta-queue-mode",
        default="local_only",
        choices=["local_only", "spill_replay", "bounded_replay"],
        help="Forwarded to benchmark_metal_gac --cta_queue_mode.",
    )
    parser.add_argument(
        "--cta-handoff-mode",
        default="push_constraints",
        choices=["push_constraints", "dirty_var_pull"],
        help="Forwarded to benchmark_metal_gac --cta_handoff_mode.",
    )
    parser.add_argument(
        "--cta-local-round-budget",
        type=int,
        default=8,
        help="Forwarded to benchmark_metal_gac --cta_local_round_budget.",
    )
    parser.add_argument(
        "--cta-replay-round-budget",
        type=int,
        default=8,
        help="Forwarded to benchmark_metal_gac --cta_replay_round_budget.",
    )
    parser.add_argument(
        "--cta-dirty-pull-min-degree",
        type=int,
        default=0,
        help="Forwarded to benchmark_metal_gac --cta_dirty_pull_min_degree.",
    )
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument(
        "--cpu-timing",
        action="store_true",
        help="Forwarded to benchmark_metal_gac --cpu_timing=true.",
    )
    parser.add_argument(
        "--cpu-warmup",
        type=int,
        default=-1,
        help="Forwarded to benchmark_metal_gac --cpu_warmup; -1 uses --warmup.",
    )
    parser.add_argument(
        "--cpu-runs",
        type=int,
        default=-1,
        help="Forwarded to benchmark_metal_gac --cpu_runs; -1 uses --runs.",
    )
    parser.add_argument("--max_iterations", type=int, default=10000)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--csv", default="out/metal_gac_v16_ablation.csv")
    parser.add_argument("--limit", type=int, default=0, help="Limit instances.")
    parser.add_argument("--verify", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--record-missing", action="store_true")
    parser.add_argument("--verbose-kernel", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()
    if args.mode_preset == "auto":
        args.kernel_variant = "auto"
        args.bitsup_layout = "auto"

    bench_bin = Path(args.bin)
    if not bench_bin.is_absolute():
        bench_bin = PROJECT_ROOT / bench_bin
    if not bench_bin.exists():
        print(f"ERROR: benchmark binary not found: {bench_bin}", file=sys.stderr)
        return 1

    try:
        modes = resolve_modes(args.mode_preset)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    tier_name, instances = resolve_instances(args)
    if args.limit > 0:
        instances = instances[: args.limit]

    rows: List[ScanRow] = []
    total_jobs = len(instances) * len(modes)
    job = 0
    for instance_index, instance in enumerate(instances, start=1):
        input_path = existing_input_path(instance)
        if not input_path.exists() and not args.record_missing:
            if not args.quiet:
                print(f"[skip] missing {rel(input_path)}")
            continue
        for mode in modes:
            job += 1
            if not args.quiet:
                print(
                    f"[{job}/{total_jobs}] {rel(input_path)} "
                    f"mode={mode[0]}+{mode[1]}",
                    flush=True,
                )
            rows.extend(
                run_one(
                    bench_bin=bench_bin,
                    instance=input_path,
                    mode=mode,
                    args=args,
                    tier_name=tier_name,
                    instance_index=instance_index,
                )
            )

    csv_path = Path(args.csv)
    if not csv_path.is_absolute():
        csv_path = PROJECT_ROOT / csv_path
    write_rows(csv_path, rows)
    summarize(rows)
    print(f"[write] {rel(csv_path)}")
    failed = [
        row
        for row in rows
        if row.status not in ("OK", "MISSING")
        and not row.error_category.startswith("unsupported_")
    ]
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
