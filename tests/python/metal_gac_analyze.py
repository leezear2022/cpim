#!/usr/bin/env python3
"""Analyze Metal GAC ablation CSV files."""

from __future__ import annotations

import argparse
import csv
import statistics
from collections import Counter, defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

Mode = Tuple[str, str]
BASELINE: Mode = ("shared", "flags")
MODE_ORDER: List[Mode] = [
    ("shared", "flags"),
    ("private", "flags"),
    ("shared", "compact"),
    ("private", "compact"),
    ("shared", "worklist"),
    ("private", "worklist"),
    ("shared", "bulk_sync_mask"),
    ("private", "bulk_sync_mask"),
    ("shared", "cta_worklist"),
    ("private", "cta_worklist"),
    ("shared", "auto"),
    ("private", "auto"),
]


def get(row: Dict[str, str], key: str, default: str = "") -> str:
    value = row.get(key)
    return default if value is None else value


def as_float(row: Dict[str, str], key: str, default: float = 0.0) -> float:
    try:
        return float(get(row, key))
    except ValueError:
        return default


def metric_ms(row: Dict[str, str]) -> float:
    value = get(row, "solve_ms")
    if value:
        try:
            return float(value)
        except ValueError:
            pass
    return as_float(row, "elapsed_ms")


def cpu_ms(row: Dict[str, str]) -> float:
    return as_float(row, "cpu_solve_ms")


def metal_cpu_ratio(row: Dict[str, str]) -> float:
    ratio = as_float(row, "metal_cpu_solve_ratio")
    if ratio > 0.0:
        return ratio
    cpu = cpu_ms(row)
    if cpu <= 0.0:
        return 0.0
    return metric_ms(row) / cpu


def cpu_timed_rows(rows: Iterable[Dict[str, str]]) -> List[Dict[str, str]]:
    return [row for row in rows if cpu_ms(row) > 0.0 and metric_ms(row) > 0.0]


def safe_ratio(numerator: float, denominator: float) -> float:
    if denominator <= 0.0:
        return 0.0
    return numerator / denominator


def parse_mode(value: str) -> Mode:
    parts = value.split("+")
    if len(parts) != 2 or not all(parts):
        raise argparse.ArgumentTypeError("mode must use '<storage>+<frontier>'")
    return (parts[0], parts[1])


def percentile(values: Sequence[float], pct: float) -> float:
    if not values:
        return 0.0
    sorted_values = sorted(values)
    if len(sorted_values) == 1:
        return sorted_values[0]
    scaled = (len(sorted_values) - 1) * pct / 100.0
    lo = int(scaled)
    hi = min(lo + 1, len(sorted_values) - 1)
    frac = scaled - lo
    return sorted_values[lo] * (1.0 - frac) + sorted_values[hi] * frac


def family_name(input_path: str) -> str:
    parts = Path(input_path).parts
    if len(parts) >= 2 and parts[0] == "benchmarks":
        return parts[1]
    if len(parts) >= 4 and parts[0] == "tests":
        return "/".join(parts[:4])
    return parts[0] if parts else input_path


def bucket_int(value: str, low: int, high: int) -> str:
    try:
        numeric = int(value)
    except ValueError:
        return "unknown"
    if numeric < low:
        return f"<{low}"
    if numeric <= high:
        return f"{low}-{high}"
    return f">{high}"


def density_bucket(value: str) -> str:
    try:
        numeric = float(value)
    except ValueError:
        return "unknown"
    if numeric < 0.10:
        return "<0.10"
    if numeric < 0.50:
        return "0.10-0.50"
    return ">=0.50"


def feature_bucket(row: Dict[str, str]) -> str:
    return (
        f"family={family_name(get(row, 'input'))} "
        f"cons={bucket_int(get(row, 'num_constraints'), 128, 511)} "
        f"dom={bucket_int(get(row, 'max_dom_size'), 17, 32)} "
        f"bitw={bucket_int(get(row, 'bit_words'), 2, 3)} "
        f"density={density_bucket(get(row, 'frontier_density_avg'))}"
    )


def classify_error(row: Dict[str, str]) -> str:
    explicit = get(row, "error_category")
    if explicit:
        return explicit
    text = get(row, "error")
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
    if "Parse/normalize failed" in text:
        return "parse_or_normalize_failure"
    if "Device layout build failed" in text:
        return "layout_build_failure"
    if "timeout" in lowered:
        return "timeout"
    return "other" if text else ""


def read_rows(path: Path) -> List[Dict[str, str]]:
    with path.open("r", newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    for row in rows:
        row["_source_csv"] = str(path)
        if "error_category" not in row:
            row["error_category"] = classify_error(row)
    return rows


def mode_of(row: Dict[str, str]) -> Mode:
    return (get(row, "readonly_storage", "shared"), get(row, "frontier_mode", "flags"))


def cta_owner_mode(row: Dict[str, str]) -> str:
    return get(row, "cta_owner_mode") or "modulo"


def cta_queue_mode(row: Dict[str, str]) -> str:
    return get(row, "cta_queue_mode") or "local_only"


def cta_handoff_mode(row: Dict[str, str]) -> str:
    return get(row, "cta_handoff_mode") or "push_constraints"


def cta_local_round_budget(row: Dict[str, str]) -> str:
    return get(row, "cta_local_round_budget") or "8"


def cta_replay_round_budget(row: Dict[str, str]) -> str:
    return get(row, "cta_replay_round_budget") or "0"


def cta_dirty_pull_min_degree(row: Dict[str, str]) -> str:
    return get(row, "cta_dirty_pull_min_degree") or "0"


def policy_mode(row: Dict[str, str]) -> str:
    return get(row, "policy_mode") or "none"


def policy_selected(row: Dict[str, str]) -> bool:
    return get(row, "policy_selected").lower() == "true"


def cta_budget_key(row: Dict[str, str]) -> Tuple[str, str]:
    return (cta_local_round_budget(row), cta_replay_round_budget(row))


def candidate_key(row: Dict[str, str]) -> Tuple[str, ...]:
    mode = mode_of(row)
    key = [
        mode[0],
        mode[1],
        get(row, "kernel_variant") or "-",
        get(row, "bitsup_layout") or "-",
        get(row, "reset_mode") or "-",
    ]
    if mode[1] == "cta_worklist":
        key.extend(
            [
                cta_owner_mode(row),
                cta_queue_mode(row),
                cta_handoff_mode(row),
                cta_local_round_budget(row),
                cta_replay_round_budget(row),
                cta_dirty_pull_min_degree(row),
            ]
        )
    return tuple(key)


def candidate_name(key: Tuple[str, ...]) -> str:
    base = f"{key[0]}+{key[1]} kernel={key[2]} bitsup={key[3]} reset={key[4]}"
    if len(key) <= 5:
        return base
    return (
        f"{base} owner={key[5]} queue={key[6]} handoff={key[7]} "
        f"local={key[8]} replay={key[9]} dirty_min={key[10]}"
    )


def path_label(row: Dict[str, str]) -> str:
    effective = "+".join(
        item
        for item in [
            get(row, "effective_frontier_mode"),
            get(row, "effective_kernel_variant"),
            get(row, "effective_bitsup_layout"),
        ]
        if item
    )
    requested = "+".join(
        item
        for item in [
            get(row, "readonly_storage"),
            get(row, "frontier_mode"),
            get(row, "kernel_variant"),
            get(row, "bitsup_layout"),
            get(row, "reset_mode"),
            get(row, "cta_owner_mode"),
            get(row, "cta_queue_mode"),
            get(row, "cta_handoff_mode"),
            get(row, "cta_local_round_budget"),
            get(row, "cta_replay_round_budget"),
            get(row, "cta_dirty_pull_min_degree"),
        ]
        if item
    )
    if effective and requested:
        return f"{requested}->{effective}"
    return requested or effective or "unknown"


def ok_rows(rows: Iterable[Dict[str, str]]) -> List[Dict[str, str]]:
    return [row for row in rows if get(row, "status", "OK") == "OK"]


def print_overview(path: Path, rows: Sequence[Dict[str, str]]) -> None:
    statuses = Counter(get(row, "status", "OK") for row in rows)
    ok = ok_rows(rows)
    error_inputs = {
        get(row, "input")
        for row in rows
        if get(row, "status", "OK") not in ("OK", "MISSING")
    }
    print(f"\n== {path} ==")
    print(
        "rows={rows} ok={ok} error={error} timeout={timeout} missing={missing} "
        "ok_instances={ok_inst} error_instances={err_inst}".format(
            rows=len(rows),
            ok=statuses["OK"],
            error=statuses["ERROR"],
            timeout=statuses["TIMEOUT"],
            missing=statuses["MISSING"],
            ok_inst=len({get(row, "input") for row in ok}),
            err_inst=len(error_inputs),
        )
    )
    if ok:
        solve = [metric_ms(row) for row in ok]
        elapsed = [as_float(row, "elapsed_ms") for row in ok]
        print(
            "solve_ms avg={avg:.3f} p50={p50:.3f} p95={p95:.3f} "
            "p99={p99:.3f} max={maxv:.3f}".format(
                avg=statistics.mean(solve),
                p50=percentile(solve, 50),
                p95=percentile(solve, 95),
                p99=percentile(solve, 99),
                maxv=max(solve),
            )
        )
        if any(elapsed):
            print(
                "elapsed_ms avg={avg:.3f} p50={p50:.3f} p95={p95:.3f} "
                "p99={p99:.3f}".format(
                    avg=statistics.mean(elapsed),
                    p50=percentile(elapsed, 50),
                    p95=percentile(elapsed, 95),
                    p99=percentile(elapsed, 99),
                )
            )
        cpu_rows = cpu_timed_rows(ok)
        if cpu_rows:
            cpu_values = [cpu_ms(row) for row in cpu_rows]
            ratios = [metal_cpu_ratio(row) for row in cpu_rows]
            metal_faster = sum(1 for ratio in ratios if ratio < 1.0)
            print(
                "cpu_solve_ms avg={avg:.6f} p50={p50:.6f} "
                "p95={p95:.6f} p99={p99:.6f}".format(
                    avg=statistics.mean(cpu_values),
                    p50=percentile(cpu_values, 50),
                    p95=percentile(cpu_values, 95),
                    p99=percentile(cpu_values, 99),
                )
            )
            print(
                "metal_cpu_solve_ratio avg={avg:.2f}x p50={p50:.2f}x "
                "p95={p95:.2f}x p99={p99:.2f}x metal_faster={faster}/{total}".format(
                    avg=statistics.mean(ratios),
                    p50=percentile(ratios, 50),
                    p95=percentile(ratios, 95),
                    p99=percentile(ratios, 99),
                    faster=metal_faster,
                    total=len(ratios),
                )
            )
        verified = sorted({get(row, "verified") for row in ok if get(row, "verified")})
        if verified:
            print(f"verified={','.join(verified)}")


def print_error_summary(rows: Sequence[Dict[str, str]], limit: int) -> None:
    failed = [
        row
        for row in rows
        if get(row, "status", "OK") not in ("OK", "MISSING")
    ]
    if not failed:
        return

    print("\n[errors]")
    by_category = Counter(classify_error(row) for row in failed)
    for category, count in by_category.most_common():
        print(f"  {category}: {count}")

    print("\n[error families]")
    by_family: Dict[str, Counter] = defaultdict(Counter)
    for row in failed:
        by_family[family_name(get(row, "input"))][classify_error(row)] += 1
    for family, counts in sorted(
        by_family.items(),
        key=lambda item: (-sum(item[1].values()), item[0]),
    )[:limit]:
        details = ", ".join(f"{key}={value}" for key, value in counts.items())
        print(f"  {family}: total={sum(counts.values())} {details}")

    print("\n[error samples]")
    seen = set()
    for row in failed:
        category = classify_error(row)
        if category in seen:
            continue
        seen.add(category)
        print(f"  {category}: {get(row, 'error')[:240]}")


def print_mode_summary(rows: Sequence[Dict[str, str]]) -> None:
    ok = ok_rows(rows)
    if not ok:
        return
    print("\n[mode summary]")
    print(
        "mode rows instances avg p50 p95 p99 setup prepare reset dispatch "
        "kernel encode wait non_kernel kernel_share dispatch_share reset_share "
        "encode_share non_kernel_share dispatch_count "
        "cpu_solve metal_cpu_ratio metal_faster "
        "frontier_density verified runners variants bitsup effective reset "
        "policy selected bucket "
        "owner queue handoff local_budget replay_budget dirty_min_degree "
        "worklist_rounds worklist_pushes worklist_push_per_round "
        "cta_local_rounds cta_pushes cta_cross cta_overflow cta_q_overflow "
        "cta_budget_spill cta_seed_overflow host_rounds "
        "cta_replay_rounds cta_replay_drain cta_replay_spill "
        "bulk_proposed bulk_actual bulk_changed_words bulk_frontier_push "
        "bulk_rounds dirty_vars dirty_scans dirty_hits cross_push_avoided "
        "dirty_fallback_push "
        "owner_map_ms owner_balance owner_weight_balance owner_local owner_cross "
        "seed_nonempty seed_empty seed_max_load seed_balance"
    )
    def cta_summary_key(
        row: Dict[str, str]
    ) -> Tuple[str, str, str, str, str, str]:
        if mode_of(row)[1] != "cta_worklist":
            return ("", "", "", "", "", "")
        return (
            cta_owner_mode(row),
            cta_queue_mode(row),
            cta_handoff_mode(row),
            cta_local_round_budget(row),
            cta_replay_round_budget(row),
            cta_dirty_pull_min_degree(row),
        )

    known_keys = sorted({(mode_of(row), cta_summary_key(row)) for row in ok})
    keys: List[Tuple[Mode, Tuple[str, str, str, str, str, str]]] = []
    for mode in MODE_ORDER:
        keys.extend(key for key in known_keys if key[0] == mode)
    keys.extend(key for key in known_keys if key not in keys)
    for mode, cta_key in keys:
        (
            owner_key,
            queue_key,
            handoff_key,
            local_budget_key,
            replay_budget_key,
            dirty_min_degree_key,
        ) = cta_key
        subset = [
            row
            for row in ok
            if mode_of(row) == mode and cta_summary_key(row) == cta_key
        ]
        elapsed = [metric_ms(row) for row in subset]
        setup = [as_float(row, "setup_ms") for row in subset]
        prepare = [as_float(row, "prepare_ms") for row in subset]
        reset = [as_float(row, "reset_ms") for row in subset]
        dispatch = [as_float(row, "dispatch_ms") for row in subset]
        kernel = [as_float(row, "kernel_ms") for row in subset]
        encode = [as_float(row, "dispatch_encode_ms") for row in subset]
        wait = [as_float(row, "dispatch_wait_ms") for row in subset]
        non_kernel = [as_float(row, "dispatch_non_kernel_ms") for row in subset]
        cpu_values = [cpu_ms(row) for row in subset if cpu_ms(row) > 0.0]
        cpu_ratios = [metal_cpu_ratio(row) for row in subset if metal_cpu_ratio(row) > 0.0]
        dispatch_counts = [as_float(row, "dispatch_count") for row in subset]
        frontier_density = [as_float(row, "frontier_density_avg") for row in subset]
        verified = sorted({get(row, "verified") for row in subset if get(row, "verified")})
        runners = sorted({get(row, "runner_mode") for row in subset if get(row, "runner_mode")})
        variants = sorted({get(row, "variant_name") for row in subset if get(row, "variant_name")})
        bitsup = sorted({get(row, "bitsup_layout") for row in subset if get(row, "bitsup_layout")})
        effective = sorted(
            {
                "+".join(
                    [
                        get(row, "effective_frontier_mode"),
                        get(row, "effective_kernel_variant"),
                        get(row, "effective_bitsup_layout"),
                    ]
                )
                for row in subset
                if get(row, "effective_frontier_mode")
            }
        )
        reset_modes = sorted({get(row, "reset_mode") for row in subset if get(row, "reset_mode")})
        policies = sorted({policy_mode(row) for row in subset if policy_mode(row)})
        selected_count = sum(1 for row in subset if policy_selected(row))
        policy_buckets = sorted(
            {get(row, "policy_bucket") for row in subset if get(row, "policy_bucket")}
        )
        worklist_rounds = [as_float(row, "worklist_rounds") for row in subset]
        worklist_pushes = [as_float(row, "worklist_push_count") for row in subset]
        cta_rounds = [as_float(row, "cta_local_rounds") for row in subset]
        cta_pushes = [as_float(row, "cta_queue_push_count") for row in subset]
        cta_cross = [as_float(row, "cta_cross_push_count") for row in subset]
        cta_overflow = [as_float(row, "cta_overflow_count") for row in subset]
        cta_q_overflow = [as_float(row, "cta_queue_overflow_count") for row in subset]
        cta_budget_spill = [as_float(row, "cta_budget_spill_count") for row in subset]
        cta_seed_overflow = [as_float(row, "cta_seed_overflow_count") for row in subset]
        cta_replay_rounds = [
            as_float(row, "cta_budget_replay_rounds") for row in subset
        ]
        cta_replay_drain = [
            as_float(row, "cta_budget_replay_drain_count") for row in subset
        ]
        cta_replay_spill = [
            as_float(row, "cta_budget_replay_spill_count") for row in subset
        ]
        bulk_proposed = [
            as_float(row, "bulk_mask_proposed_deletion_count") for row in subset
        ]
        bulk_actual = [
            as_float(row, "bulk_mask_actual_deletion_count") for row in subset
        ]
        bulk_changed_words = [
            as_float(row, "bulk_mask_changed_word_count") for row in subset
        ]
        bulk_frontier_push = [
            as_float(row, "bulk_mask_frontier_push_count") for row in subset
        ]
        bulk_rounds = [as_float(row, "bulk_mask_rounds") for row in subset]
        dirty_vars = [as_float(row, "dirty_var_count") for row in subset]
        dirty_scans = [as_float(row, "dirty_pull_scan_count") for row in subset]
        dirty_hits = [as_float(row, "dirty_pull_hit_count") for row in subset]
        cross_push_avoided = [
            as_float(row, "cross_push_avoided_count") for row in subset
        ]
        dirty_fallback_push = [
            as_float(row, "dirty_pull_fallback_push_count") for row in subset
        ]
        host_rounds = [as_float(row, "host_round_count") for row in subset]
        owner_map_ms = [as_float(row, "owner_map_build_ms") for row in subset]
        owner_balance = [as_float(row, "owner_balance_p95") for row in subset]
        owner_weight_balance = [
            as_float(row, "owner_weight_balance_p95") for row in subset
        ]
        owner_local = [as_float(row, "owner_local_push_count") for row in subset]
        owner_cross = [as_float(row, "owner_cross_push_count") for row in subset]
        seed_nonempty = [as_float(row, "seed_owner_nonempty_count") for row in subset]
        seed_empty = [as_float(row, "seed_empty_owner_count") for row in subset]
        seed_max_load = [as_float(row, "seed_max_owner_load") for row in subset]
        seed_balance = [as_float(row, "seed_owner_balance_p95") for row in subset]
        avg_reset = statistics.mean(reset)
        avg_dispatch = statistics.mean(dispatch)
        avg_kernel = statistics.mean(kernel)
        avg_encode = statistics.mean(encode)
        avg_wait = statistics.mean(wait)
        avg_non_kernel = statistics.mean(non_kernel)
        avg_solve = statistics.mean(elapsed)
        avg_cpu = statistics.mean(cpu_values) if cpu_values else 0.0
        ratio_p50 = percentile(cpu_ratios, 50) if cpu_ratios else 0.0
        metal_faster = sum(1 for ratio in cpu_ratios if ratio < 1.0)
        avg_rounds = statistics.mean(worklist_rounds) if any(worklist_rounds) else 0.0
        avg_pushes = statistics.mean(worklist_pushes) if any(worklist_pushes) else 0.0
        print(
            "{mode} {rows} {inst} {avg:.3f} {p50:.3f} {p95:.3f} {p99:.3f} "
            "{setup:.3f} {prepare:.3f} {reset:.3f} {dispatch:.3f} "
            "{kernel:.3f} {encode:.3f} {wait:.3f} {non_kernel:.3f} "
            "{kernel_share:.2f} {dispatch_share:.2f} "
            "{reset_share:.2f} {encode_share:.2f} {non_kernel_share:.2f} "
            "{dc:.2f} {cpu:.3f} {ratio:.2f} "
            "{metal_faster}/{cpu_rows} {density:.3f} {verified} {runners} "
            "{variants} {bitsup} {effective} {reset_modes} {policy} "
            "{selected} {bucket} {owner} {queue} "
            "{handoff} {local_budget} {replay_budget} {dirty_min_degree} "
            "{wl_rounds:.2f} {wl_pushes:.2f} {push_per_round:.2f} "
            "{cta_rounds:.2f} {cta_pushes:.2f} {cta_cross:.2f} "
            "{cta_overflow:.2f} {cta_q_overflow:.2f} {cta_budget_spill:.2f} "
            "{cta_seed_overflow:.2f} {host_rounds:.2f} "
            "{cta_replay_rounds:.2f} {cta_replay_drain:.2f} "
            "{cta_replay_spill:.2f} {bulk_proposed:.2f} {bulk_actual:.2f} "
            "{bulk_changed_words:.2f} {bulk_frontier_push:.2f} "
            "{bulk_rounds:.2f} {dirty_vars:.2f} {dirty_scans:.2f} "
            "{dirty_hits:.2f} {cross_push_avoided:.2f} "
            "{dirty_fallback_push:.2f} {owner_map_ms:.3f} "
            "{owner_balance:.2f} {owner_weight_balance:.2f} "
            "{owner_local:.2f} {owner_cross:.2f} {seed_nonempty:.2f} "
            "{seed_empty:.2f} {seed_max_load:.2f} {seed_balance:.2f}".format(
                mode=f"{mode[0]}+{mode[1]}",
                rows=len(subset),
                inst=len({get(row, "input") for row in subset}),
                avg=avg_solve,
                p50=percentile(elapsed, 50),
                p95=percentile(elapsed, 95),
                p99=percentile(elapsed, 99),
                setup=statistics.mean(setup),
                prepare=statistics.mean(prepare),
                reset=avg_reset,
                dispatch=avg_dispatch,
                kernel=avg_kernel,
                encode=avg_encode,
                wait=avg_wait,
                non_kernel=avg_non_kernel,
                kernel_share=safe_ratio(avg_kernel, avg_solve),
                dispatch_share=safe_ratio(avg_dispatch, avg_solve),
                reset_share=safe_ratio(avg_reset, avg_solve),
                encode_share=safe_ratio(avg_encode, avg_solve),
                non_kernel_share=safe_ratio(avg_non_kernel, avg_solve),
                dc=statistics.mean(dispatch_counts) if any(dispatch_counts) else 0.0,
                cpu=avg_cpu,
                ratio=ratio_p50,
                metal_faster=metal_faster,
                cpu_rows=len(cpu_ratios),
                density=statistics.mean(frontier_density)
                if any(frontier_density)
                else 0.0,
                verified="/".join(verified),
                runners="/".join(runners),
                variants="/".join(variants),
                bitsup="/".join(bitsup),
                effective="/".join(effective),
                reset_modes="/".join(reset_modes),
                policy="/".join(policies),
                selected=selected_count,
                bucket="/".join(policy_buckets) if policy_buckets else "-",
                owner=owner_key or "-",
                queue=queue_key or "-",
                handoff=handoff_key or "-",
                local_budget=local_budget_key or "-",
                replay_budget=replay_budget_key or "-",
                dirty_min_degree=dirty_min_degree_key or "-",
                wl_rounds=avg_rounds,
                wl_pushes=avg_pushes,
                push_per_round=safe_ratio(avg_pushes, avg_rounds),
                cta_rounds=statistics.mean(cta_rounds) if any(cta_rounds) else 0.0,
                cta_pushes=statistics.mean(cta_pushes) if any(cta_pushes) else 0.0,
                cta_cross=statistics.mean(cta_cross) if any(cta_cross) else 0.0,
                cta_overflow=statistics.mean(cta_overflow)
                if any(cta_overflow)
                else 0.0,
                cta_q_overflow=statistics.mean(cta_q_overflow)
                if any(cta_q_overflow)
                else 0.0,
                cta_budget_spill=statistics.mean(cta_budget_spill)
                if any(cta_budget_spill)
                else 0.0,
                cta_seed_overflow=statistics.mean(cta_seed_overflow)
                if any(cta_seed_overflow)
                else 0.0,
                host_rounds=statistics.mean(host_rounds) if any(host_rounds) else 0.0,
                cta_replay_rounds=statistics.mean(cta_replay_rounds)
                if any(cta_replay_rounds)
                else 0.0,
                cta_replay_drain=statistics.mean(cta_replay_drain)
                if any(cta_replay_drain)
                else 0.0,
                cta_replay_spill=statistics.mean(cta_replay_spill)
                if any(cta_replay_spill)
                else 0.0,
                bulk_proposed=statistics.mean(bulk_proposed)
                if any(bulk_proposed)
                else 0.0,
                bulk_actual=statistics.mean(bulk_actual) if any(bulk_actual) else 0.0,
                bulk_changed_words=statistics.mean(bulk_changed_words)
                if any(bulk_changed_words)
                else 0.0,
                bulk_frontier_push=statistics.mean(bulk_frontier_push)
                if any(bulk_frontier_push)
                else 0.0,
                bulk_rounds=statistics.mean(bulk_rounds) if any(bulk_rounds) else 0.0,
                dirty_vars=statistics.mean(dirty_vars) if any(dirty_vars) else 0.0,
                dirty_scans=statistics.mean(dirty_scans) if any(dirty_scans) else 0.0,
                dirty_hits=statistics.mean(dirty_hits) if any(dirty_hits) else 0.0,
                cross_push_avoided=statistics.mean(cross_push_avoided)
                if any(cross_push_avoided)
                else 0.0,
                dirty_fallback_push=statistics.mean(dirty_fallback_push)
                if any(dirty_fallback_push)
                else 0.0,
                owner_map_ms=statistics.mean(owner_map_ms)
                if any(owner_map_ms)
                else 0.0,
                owner_balance=statistics.mean(owner_balance)
                if any(owner_balance)
                else 0.0,
                owner_weight_balance=statistics.mean(owner_weight_balance)
                if any(owner_weight_balance)
                else 0.0,
                owner_local=statistics.mean(owner_local) if any(owner_local) else 0.0,
                owner_cross=statistics.mean(owner_cross) if any(owner_cross) else 0.0,
                seed_nonempty=statistics.mean(seed_nonempty)
                if any(seed_nonempty)
                else 0.0,
                seed_empty=statistics.mean(seed_empty) if any(seed_empty) else 0.0,
                seed_max_load=statistics.mean(seed_max_load)
                if any(seed_max_load)
                else 0.0,
                seed_balance=statistics.mean(seed_balance)
                if any(seed_balance)
                else 0.0,
            )
        )


def print_ratio_summary(rows: Sequence[Dict[str, str]], limit: int) -> None:
    ok = ok_rows(rows)
    inputs = sorted({get(row, "input") for row in ok})
    ratios: Dict[Mode, List[Tuple[float, str]]] = defaultdict(list)
    for input_path in inputs:
        per_mode: Dict[Mode, float] = {}
        for mode in sorted({mode_of(row) for row in ok}):
            subset = [
                row
                for row in ok
                if get(row, "input") == input_path and mode_of(row) == mode
            ]
            if subset:
                per_mode[mode] = statistics.mean(
                    metric_ms(row) for row in subset
                )
        baseline = per_mode.get(BASELINE)
        if not baseline or baseline <= 0.0:
            continue
        for mode, elapsed in per_mode.items():
            if mode == BASELINE:
                continue
            ratios[mode].append((elapsed / baseline, input_path))

    if not ratios:
        return

    print("\n[relative to shared+flags]")
    for mode in MODE_ORDER:
        if mode == BASELINE or mode not in ratios:
            continue
        values = [value for value, _ in ratios[mode]]
        print(
            "{mode} avg={avg:.2f}x p50={p50:.2f}x better={better}/{total}".format(
                mode=f"{mode[0]}+{mode[1]}",
                avg=statistics.mean(values),
                p50=percentile(values, 50),
                better=sum(1 for value in values if value < 1.0),
                total=len(values),
            )
        )
        for value, input_path in sorted(ratios[mode])[:limit]:
            print(f"  best {value:.2f}x {input_path}")


def print_metal_vs_cpu(rows: Sequence[Dict[str, str]], limit: int) -> None:
    ok = cpu_timed_rows(ok_rows(rows))
    if not ok:
        return

    print("\n[metal vs cpu]")
    print("ratio < 1.0 means Metal is faster than CPU for GAC solve time.")

    known_modes = sorted({mode_of(row) for row in ok})
    modes = [mode for mode in MODE_ORDER if mode in known_modes]
    modes.extend(mode for mode in known_modes if mode not in modes)
    for mode in modes:
        subset = [row for row in ok if mode_of(row) == mode]
        ratios = [metal_cpu_ratio(row) for row in subset]
        cpu_values = [cpu_ms(row) for row in subset]
        metal_values = [metric_ms(row) for row in subset]
        faster = sum(1 for ratio in ratios if ratio < 1.0)
        print(
            "{mode} rows={rows} instances={inst} metal_faster={faster}/{rows} "
            "ratio_avg={avg:.2f}x ratio_p50={p50:.2f}x "
            "ratio_p95={p95:.2f}x ratio_p99={p99:.2f}x "
            "metal_p50={metal_p50:.3f} cpu_p50={cpu_p50:.6f}".format(
                mode=f"{mode[0]}+{mode[1]}",
                rows=len(subset),
                inst=len({get(row, "input") for row in subset}),
                faster=faster,
                avg=statistics.mean(ratios),
                p50=percentile(ratios, 50),
                p95=percentile(ratios, 95),
                p99=percentile(ratios, 99),
                metal_p50=percentile(metal_values, 50),
                cpu_p50=percentile(cpu_values, 50),
            )
        )

        regressions = sorted(
            [(ratio, row) for ratio, row in zip(ratios, subset)],
            key=lambda item: item[0],
            reverse=True,
        )
        for ratio, row in regressions[:limit]:
            print(
                "  worst {ratio:.2f}x input={input} metal_ms={metal:.3f} "
                "cpu_ms={cpu:.6f} path={path}".format(
                    ratio=ratio,
                    input=get(row, "input"),
                    metal=metric_ms(row),
                    cpu=cpu_ms(row),
                    path=path_label(row),
                )
            )


def print_recommended_policy_summary(rows: Sequence[Dict[str, str]],
                                     limit: int,
                                     baseline_mode: Mode = BASELINE,
                                     regression_threshold: float = 1.05,
                                     min_runs: int = 1) -> None:
    ok = ok_rows(rows)
    if not ok:
        return

    by_input: Dict[str, List[Dict[str, str]]] = defaultdict(list)
    for row in ok:
        by_input[get(row, "input")].append(row)

    best_counts: Counter = Counter()
    bucket_counts: Dict[str, Counter] = defaultdict(Counter)
    auto_hits = 0
    auto_wins = 0
    auto_regressions: List[Tuple[float, str, Dict[str, str]]] = []
    compared = 0

    for input_path, subset in by_input.items():
        per_mode: Dict[Mode, float] = {}
        sample_by_mode: Dict[Mode, Dict[str, str]] = {}
        for mode in sorted({mode_of(row) for row in subset}):
            mode_rows = [row for row in subset if mode_of(row) == mode]
            if len(mode_rows) < min_runs:
                continue
            per_mode[mode] = statistics.mean(metric_ms(row) for row in mode_rows)
            sample_by_mode[mode] = mode_rows[0]
        if not per_mode:
            continue
        best_mode = min(per_mode, key=per_mode.get)
        best_counts[f"{best_mode[0]}+{best_mode[1]}"] += 1
        bucket_counts[feature_bucket(sample_by_mode[best_mode])][
            f"{best_mode[0]}+{best_mode[1]}"
        ] += 1

        baseline = per_mode.get(baseline_mode)
        auto_mode = ("shared", "auto")
        auto_value = per_mode.get(auto_mode)
        if baseline is None or auto_value is None or baseline <= 0.0:
            continue
        compared += 1
        auto_hits += 1
        if auto_value <= baseline:
            auto_wins += 1
        elif auto_value > baseline * regression_threshold:
            ratio = auto_value / baseline
            auto_regressions.append(
                (ratio, input_path, sample_by_mode.get(auto_mode, subset[0]))
            )

    if not best_counts and compared == 0:
        return

    print("\n[recommended policy summary]")
    if best_counts:
        details = ", ".join(
            f"{mode}={count}" for mode, count in best_counts.most_common()
        )
        print(f"best_mode_counts {details}")
    if compared:
        print(
            "auto_vs_{baseline} compared={compared} hits={hits} "
            "wins={wins} regressions_gt_5pct={reg}".format(
                baseline=f"{baseline_mode[0]}+{baseline_mode[1]}",
                compared=compared,
                hits=auto_hits,
                wins=auto_wins,
                reg=len(auto_regressions),
            )
        )

    if bucket_counts:
        print("[recommended buckets]")
        ranked = sorted(
            bucket_counts.items(),
            key=lambda item: (-sum(item[1].values()), item[0]),
        )
        for bucket, counts in ranked[:limit]:
            winner = counts.most_common(1)[0][0]
            details = ", ".join(
                f"{mode}={count}" for mode, count in counts.most_common(3)
            )
            print(f"  {bucket}: recommend={winner} {details}")

    if auto_regressions:
        print("[auto regressions >5%]")
        for ratio, input_path, row in sorted(auto_regressions, reverse=True)[:limit]:
            print(
                "  {ratio:.2f}x {input} effective={effective} "
                "cons={cons} dom={dom} bitw={bitw} density={density}".format(
                    ratio=ratio,
                    input=input_path,
                    effective="+".join(
                        [
                            get(row, "effective_frontier_mode"),
                            get(row, "effective_kernel_variant"),
                            get(row, "effective_bitsup_layout"),
                        ]
                    ),
                    cons=get(row, "num_constraints"),
                    dom=get(row, "max_dom_size"),
                    bitw=get(row, "bit_words"),
                    density=get(row, "frontier_density_avg"),
                )
            )


def print_policy_recommendations(rows: Sequence[Dict[str, str]],
                                 limit: int,
                                 baseline_mode: Mode,
                                 regression_threshold: float,
                                 min_runs: int) -> None:
    ok = ok_rows(rows)
    if not ok:
        return

    by_input: Dict[str, List[Dict[str, str]]] = defaultdict(list)
    for row in ok:
        by_input[get(row, "input")].append(row)

    bucket_modes: Dict[str, Dict[Mode, List[Tuple[float, Dict[str, str]]]]] = (
        defaultdict(lambda: defaultdict(list))
    )
    global_modes: Dict[Mode, List[Tuple[float, Dict[str, str]]]] = defaultdict(list)
    bottlenecks: Counter = Counter()

    for input_path, subset in by_input.items():
        mode_rows: Dict[Mode, List[Dict[str, str]]] = defaultdict(list)
        for row in subset:
            mode_rows[mode_of(row)].append(row)
        baseline_rows = mode_rows.get(baseline_mode, [])
        if len(baseline_rows) < min_runs:
            continue
        baseline_value = statistics.mean(metric_ms(row) for row in baseline_rows)
        if baseline_value <= 0.0:
            continue

        sample = baseline_rows[0]
        bucket = feature_bucket(sample)
        avg_kernel = statistics.mean(as_float(row, "kernel_ms") for row in baseline_rows)
        avg_dispatch = statistics.mean(as_float(row, "dispatch_ms") for row in baseline_rows)
        avg_reset = statistics.mean(as_float(row, "reset_ms") for row in baseline_rows)
        bottlenecks[
            max(
                [("kernel", avg_kernel), ("dispatch", avg_dispatch), ("reset", avg_reset)],
                key=lambda item: item[1],
            )[0]
        ] += 1

        for mode, candidates in mode_rows.items():
            if len(candidates) < min_runs:
                continue
            candidate_value = statistics.mean(metric_ms(row) for row in candidates)
            ratio = candidate_value / baseline_value
            bucket_modes[bucket][mode].append((ratio, candidates[0]))
            global_modes[mode].append((ratio, candidates[0]))

    if not bucket_modes and not global_modes:
        return

    def confidence(count: int, regressions: int) -> str:
        if count >= 5 and regressions == 0:
            return "high"
        if count >= 3 and regressions <= max(1, count // 10):
            return "medium"
        return "low"

    def summarize_mode(values: Sequence[Tuple[float, Dict[str, str]]]) -> Tuple[float, float, int]:
        ratios = [ratio for ratio, _ in values]
        return percentile(ratios, 50), percentile(ratios, 95), sum(
            1 for ratio in ratios if ratio > regression_threshold
        )

    print("\n[v3 policy recommendations]")
    if bottlenecks:
        details = ", ".join(f"{name}={count}" for name, count in bottlenecks.most_common())
        print(f"baseline_bottleneck_counts {details}")

    print("[global candidates]")
    ranked_global = []
    for mode, values in global_modes.items():
        if mode == baseline_mode or not values:
            continue
        p50, p95, regressions = summarize_mode(values)
        ranked_global.append((p95 > regression_threshold, p50, p95, mode, values, regressions))
    for _, p50, p95, mode, values, regressions in sorted(ranked_global)[:limit]:
        sample = values[0][1]
        print(
            "  recommend={mode} confidence={conf} compared={count} "
            "p50_ratio={p50:.2f} p95_ratio={p95:.2f} regressions_gt_threshold={reg} "
            "path={path}".format(
                mode=f"{mode[0]}+{mode[1]}",
                conf=confidence(len(values), regressions),
                count=len(values),
                p50=p50,
                p95=p95,
                reg=regressions,
                path=path_label(sample),
            )
        )

    print("[bucket candidates]")
    ranked_buckets = sorted(
        bucket_modes.items(),
        key=lambda item: (-sum(len(values) for values in item[1].values()), item[0]),
    )
    for bucket, modes in ranked_buckets[:limit]:
        choices = []
        for mode, values in modes.items():
            if mode == baseline_mode or not values:
                continue
            p50, p95, regressions = summarize_mode(values)
            choices.append((p95 > regression_threshold, p50, p95, mode, values, regressions))
        if not choices:
            continue
        _, p50, p95, mode, values, regressions = sorted(choices)[0]
        print(
            "  {bucket}: recommend={mode} confidence={conf} compared={count} "
            "p50_ratio={p50:.2f} p95_ratio={p95:.2f} regressions_gt_threshold={reg}".format(
                bucket=bucket,
                mode=f"{mode[0]}+{mode[1]}",
                conf=confidence(len(values), regressions),
                count=len(values),
                p50=p50,
                p95=p95,
                reg=regressions,
            )
        )

    regressions = []
    for mode, values in global_modes.items():
        if mode == baseline_mode:
            continue
        for ratio, row in values:
            if ratio > regression_threshold:
                regressions.append((ratio, mode, row))
    if regressions:
        print("[policy regressions]")
        for ratio, mode, row in sorted(regressions, reverse=True)[:limit]:
            print(
                "  {ratio:.2f}x mode={mode} input={input} path={path} "
                "cons={cons} dom={dom} bitw={bitw} density={density}".format(
                    ratio=ratio,
                    mode=f"{mode[0]}+{mode[1]}",
                    input=get(row, "input"),
                    path=path_label(row),
                    cons=get(row, "num_constraints"),
                    dom=get(row, "max_dom_size"),
                    bitw=get(row, "bit_words"),
                    density=get(row, "frontier_density_avg"),
                )
            )


def print_bucket_policy_simulation(rows: Sequence[Dict[str, str]],
                                   limit: int,
                                   fallback_mode: Mode,
                                   regression_threshold: float,
                                   min_runs: int,
                                   bucket_min_instances: int) -> None:
    ok = ok_rows(rows)
    if not ok:
        return

    by_input: Dict[str, List[Dict[str, str]]] = defaultdict(list)
    for row in ok:
        by_input[get(row, "input")].append(row)

    bucket_candidates: Dict[
        str, Dict[Tuple[str, ...], List[Tuple[float, str, Dict[str, str]]]]
    ] = defaultdict(lambda: defaultdict(list))
    per_input: Dict[
        str,
        Tuple[
            str,
            float,
            Dict[Tuple[str, ...], float],
            Dict[Tuple[str, ...], Dict[str, str]],
        ],
    ] = {}

    for input_path, subset in by_input.items():
        grouped: Dict[Tuple[str, ...], List[Dict[str, str]]] = defaultdict(list)
        for row in subset:
            grouped[candidate_key(row)].append(row)

        candidate_means: Dict[Tuple[str, ...], float] = {}
        candidate_samples: Dict[Tuple[str, ...], Dict[str, str]] = {}
        for key, candidate_rows in grouped.items():
            if len(candidate_rows) < min_runs:
                continue
            value = statistics.mean(metric_ms(row) for row in candidate_rows)
            if value <= 0.0:
                continue
            candidate_means[key] = value
            candidate_samples[key] = candidate_rows[0]

        fallback_choices = [
            (value, key)
            for key, value in candidate_means.items()
            if key[0] == fallback_mode[0] and key[1] == fallback_mode[1]
        ]
        if not fallback_choices:
            continue
        fallback_value, fallback_key = min(fallback_choices)
        if fallback_value <= 0.0:
            continue

        sample = candidate_samples[fallback_key]
        bucket = feature_bucket(sample)
        candidate_values: Dict[Tuple[str, ...], float] = {}
        selected_samples: Dict[Tuple[str, ...], Dict[str, str]] = {}
        for key, candidate_value in candidate_means.items():
            if key[0] == fallback_mode[0] and key[1] == fallback_mode[1]:
                continue
            candidate_values[key] = candidate_value
            selected_samples[key] = candidate_samples[key]
            bucket_candidates[bucket][key].append(
                (candidate_value / fallback_value, input_path, candidate_samples[key])
            )
        per_input[input_path] = (
            bucket,
            fallback_value,
            candidate_values,
            selected_samples,
        )

    if not per_input:
        return

    eligible: Dict[
        str, Tuple[Tuple[str, ...], float, float, int, int, Dict[str, str]]
    ] = {}
    rejected: List[Tuple[str, Tuple[str, ...], float, float, int, int]] = []
    for bucket, candidates in bucket_candidates.items():
        choices = []
        for key, values in candidates.items():
            if len(values) < bucket_min_instances:
                continue
            ratios = [ratio for ratio, _, _ in values]
            p50 = percentile(ratios, 50)
            p95 = percentile(ratios, 95)
            regressions = sum(1 for ratio in ratios if ratio > regression_threshold)
            choices.append((regressions > 0, p95, p50, key, values, regressions))
            if regressions > 0:
                rejected.append((bucket, key, p50, p95, regressions, len(values)))
        if not choices:
            continue
        has_safe_choice, p95, p50, key, values, regressions = sorted(choices)[0]
        if not has_safe_choice and p95 <= 1.0:
            eligible[bucket] = (key, p50, p95, regressions, len(values), values[0][2])

    if not eligible:
        print("\n[bucket policy simulation]")
        print(
            "fallback={fallback} eligible_buckets=0 inputs={inputs}".format(
                fallback=f"{fallback_mode[0]}+{fallback_mode[1]}",
                inputs=len(per_input),
            )
        )
        return

    policy_ratios: List[float] = []
    policy_values: List[float] = []
    fallback_values: List[float] = []
    selected_count = 0
    selected_by_bucket: Counter = Counter()
    regressions: List[Tuple[float, str, str, Tuple[str, ...], Dict[str, str]]] = []
    for input_path, (bucket, fallback_value, candidate_values, samples) in per_input.items():
        selected_key = None
        selected_value = fallback_value
        if bucket in eligible:
            key = eligible[bucket][0]
            if key in candidate_values:
                selected_key = key
                selected_value = candidate_values[key]
        ratio = selected_value / fallback_value
        policy_ratios.append(ratio)
        policy_values.append(selected_value)
        fallback_values.append(fallback_value)
        if selected_key is not None:
            selected_count += 1
            selected_by_bucket[bucket] += 1
            if ratio > regression_threshold:
                regressions.append(
                    (ratio, input_path, bucket, selected_key, samples[selected_key])
                )

    print("\n[bucket policy simulation]")
    print(
        "fallback={fallback} eligible_buckets={eligible} selected_inputs={selected} "
        "inputs={inputs}".format(
            fallback=f"{fallback_mode[0]}+{fallback_mode[1]}",
            eligible=len(eligible),
            selected=selected_count,
            inputs=len(per_input),
        )
    )
    print(
        "policy_ms p50={p50:.3f} p95={p95:.3f} fallback_p50={fb50:.3f} "
        "fallback_p95={fb95:.3f}".format(
            p50=percentile(policy_values, 50),
            p95=percentile(policy_values, 95),
            fb50=percentile(fallback_values, 50),
            fb95=percentile(fallback_values, 95),
        )
    )
    print(
        "policy_vs_fallback p50={p50:.2f}x p95={p95:.2f}x "
        "better={better}/{total} regressions_gt_threshold={reg}".format(
            p50=percentile(policy_ratios, 50),
            p95=percentile(policy_ratios, 95),
            better=sum(1 for ratio in policy_ratios if ratio < 1.0),
            total=len(policy_ratios),
            reg=len(regressions),
        )
    )
    print("[eligible buckets]")
    for bucket, (key, p50, p95, regressions_count, count, sample) in sorted(
        eligible.items(),
        key=lambda item: (-item[1][4], item[0]),
    )[:limit]:
        print(
            "  {bucket}: recommend={candidate} compared={count} "
            "p50={p50:.2f}x p95={p95:.2f}x selected={selected} path={path}".format(
                bucket=bucket,
                candidate=candidate_name(key),
                count=count,
                p50=p50,
                p95=p95,
                selected=selected_by_bucket[bucket],
                path=path_label(sample),
            )
        )
    if regressions:
        print("[bucket policy regressions]")
        for ratio, input_path, bucket, key, row in sorted(regressions, reverse=True)[:limit]:
            print(
                "  {ratio:.2f}x input={input} bucket={bucket} candidate={candidate} "
                "path={path}".format(
                    ratio=ratio,
                    input=input_path,
                    bucket=bucket,
                    candidate=candidate_name(key),
                    path=path_label(row),
                )
            )


def print_runtime_policy_summary(rows: Sequence[Dict[str, str]],
                                 limit: int,
                                 fallback_mode: Mode,
                                 regression_threshold: float,
                                 min_runs: int) -> None:
    ok = [
        row for row in ok_rows(rows)
        if policy_mode(row) != "none" or get(row, "policy_selected")
    ]
    if not ok:
        return

    print("\n[runtime policy summary]")
    by_policy = Counter(policy_mode(row) for row in ok)
    details = ", ".join(f"{key}={value}" for key, value in sorted(by_policy.items()))
    print(f"rows={len(ok)} policies={details}")
    selected = [row for row in ok if policy_selected(row)]
    not_selected = [row for row in ok if not policy_selected(row)]
    reason_counts = Counter(get(row, "policy_reason") for row in ok)
    reason_details = ", ".join(
        f"{key or '-'}={value}" for key, value in reason_counts.most_common()
    )
    print(
        "selected_rows={selected} selected_inputs={selected_inputs} "
        "not_selected_rows={not_selected} reasons={reasons}".format(
            selected=len(selected),
            selected_inputs=len({get(row, "input") for row in selected}),
            not_selected=len(not_selected),
            reasons=reason_details,
        )
    )
    if selected:
        buckets = Counter(get(row, "policy_bucket") for row in selected)
        bucket_details = ", ".join(
            f"{key or '-'}={value}" for key, value in buckets.most_common()
        )
        print(f"selected_buckets {bucket_details}")

    by_input: Dict[str, List[Dict[str, str]]] = defaultdict(list)
    for row in ok_rows(rows):
        by_input[get(row, "input")].append(row)

    ratios: List[Tuple[float, str, Dict[str, str]]] = []
    for input_path, subset in by_input.items():
        selected_rows = [
            row for row in subset
            if policy_selected(row) and policy_mode(row) != "none"
        ]
        fallback_rows = [
            row for row in subset
            if (
                mode_of(row) == fallback_mode and
                not policy_selected(row) and
                policy_mode(row) == "none"
            )
        ]
        if len(selected_rows) < min_runs or len(fallback_rows) < min_runs:
            continue
        selected_ms = statistics.mean(metric_ms(row) for row in selected_rows)
        fallback_ms = statistics.mean(metric_ms(row) for row in fallback_rows)
        if selected_ms > 0.0 and fallback_ms > 0.0:
            ratios.append((selected_ms / fallback_ms, input_path, selected_rows[0]))

    if not ratios:
        return
    values = [ratio for ratio, _, _ in ratios]
    regressions = [
        (ratio, input_path, row)
        for ratio, input_path, row in ratios
        if ratio > regression_threshold
    ]
    promote_signal = (
        bool(values) and
        percentile(values, 50) < 1.0 and
        percentile(values, 95) <= regression_threshold and
        not regressions
    )
    print(
        "selected_vs_{fallback} compared={count} p50={p50:.2f}x "
        "p95={p95:.2f}x better={better}/{count} regressions_gt_threshold={reg}".format(
            fallback=f"{fallback_mode[0]}+{fallback_mode[1]}",
            count=len(values),
            p50=percentile(values, 50),
            p95=percentile(values, 95),
            better=sum(1 for value in values if value < 1.0),
            reg=len(regressions),
        )
    )
    print(
        "decision={decision} reason={reason}".format(
            decision="eligible" if promote_signal else "report_only",
            reason="none" if promote_signal else "insufficient_or_regression",
        )
    )
    if regressions:
        print("[runtime policy regressions]")
        for ratio, input_path, row in sorted(regressions, reverse=True)[:limit]:
            print(
                "  {ratio:.2f}x input={input} reason={reason} bucket={bucket} "
                "path={path}".format(
                    ratio=ratio,
                    input=input_path,
                    reason=get(row, "policy_reason"),
                    bucket=get(row, "policy_bucket"),
                    path=path_label(row),
                )
            )


def print_dispatch_timing_split(rows: Sequence[Dict[str, str]]) -> None:
    ok = ok_rows(rows)
    if not ok:
        return
    timed = [
        row for row in ok
        if (
            as_float(row, "dispatch_encode_ms") > 0.0 or
            as_float(row, "dispatch_wait_ms") > 0.0 or
            as_float(row, "dispatch_non_kernel_ms") > 0.0
        )
    ]
    if not timed:
        return

    print("\n[dispatch timing split]")
    print(
        "mode rows instances encode_ms wait_ms kernel_ms non_kernel_ms "
        "encode_per_dispatch wait_per_dispatch kernel_per_dispatch "
        "non_kernel_per_dispatch non_kernel_share"
    )
    known_keys = sorted({(mode_of(row), candidate_key(row)) for row in timed})
    ordered: List[Tuple[Mode, Tuple[str, ...]]] = []
    for mode in MODE_ORDER:
        ordered.extend(key for key in known_keys if key[0] == mode)
    ordered.extend(key for key in known_keys if key not in ordered)
    for mode, key in ordered:
        subset = [
            row for row in timed
            if mode_of(row) == mode and candidate_key(row) == key
        ]
        dispatch_counts = [as_float(row, "dispatch_count") for row in subset]
        dispatches = statistics.mean(dispatch_counts) if any(dispatch_counts) else 0.0
        encode = [as_float(row, "dispatch_encode_ms") for row in subset]
        wait = [as_float(row, "dispatch_wait_ms") for row in subset]
        kernel = [as_float(row, "kernel_ms") for row in subset]
        non_kernel = [as_float(row, "dispatch_non_kernel_ms") for row in subset]
        avg_encode = statistics.mean(encode)
        avg_wait = statistics.mean(wait)
        avg_kernel = statistics.mean(kernel)
        avg_non_kernel = statistics.mean(non_kernel)
        print(
            "{mode} rows={rows} instances={inst} encode={encode:.3f} "
            "wait={wait:.3f} kernel={kernel:.3f} non_kernel={non_kernel:.3f} "
            "encode_per_dispatch={encode_pd:.4f} wait_per_dispatch={wait_pd:.4f} "
            "kernel_per_dispatch={kernel_pd:.4f} non_kernel_per_dispatch={non_pd:.4f} "
            "non_kernel_share={share:.2f}".format(
                mode=candidate_name(key),
                rows=len(subset),
                inst=len({get(row, "input") for row in subset}),
                encode=avg_encode,
                wait=avg_wait,
                kernel=avg_kernel,
                non_kernel=avg_non_kernel,
                encode_pd=safe_ratio(avg_encode, dispatches),
                wait_pd=safe_ratio(avg_wait, dispatches),
                kernel_pd=safe_ratio(avg_kernel, dispatches),
                non_pd=safe_ratio(avg_non_kernel, dispatches),
                share=safe_ratio(avg_non_kernel, avg_wait),
            )
        )


def round_count(row: Dict[str, str]) -> float:
    host_rounds = as_float(row, "host_round_count")
    if host_rounds > 0.0:
        return host_rounds
    iterations = as_float(row, "iterations")
    if iterations > 0.0:
        return iterations
    return as_float(row, "dispatch_count")


def mean_metric(rows: Sequence[Dict[str, str]], metric) -> float:
    if not rows:
        return 0.0
    return statistics.mean(metric(row) for row in rows)


def print_cta_worklist_gate(rows: Sequence[Dict[str, str]],
                            limit: int,
                            baseline_mode: Mode,
                            regression_threshold: float,
                            min_runs: int) -> None:
    ok = ok_rows(rows)
    cta_mode: Mode = ("shared", "cta_worklist")
    if not any(mode_of(row) == cta_mode for row in ok):
        return
    cta_modes = sorted(
        {
            (
                cta_owner_mode(row),
                cta_queue_mode(row),
                cta_handoff_mode(row),
                cta_local_round_budget(row),
                cta_replay_round_budget(row),
                cta_dirty_pull_min_degree(row),
            )
            for row in ok
            if mode_of(row) == cta_mode
        }
    )
    for (
        owner_mode,
        queue_mode,
        handoff_mode,
        local_budget,
        replay_budget,
        dirty_min_degree,
    ) in cta_modes:
        print_cta_worklist_gate_for_cta_mode(
            rows,
            limit,
            baseline_mode=baseline_mode,
            regression_threshold=regression_threshold,
            min_runs=min_runs,
            owner_mode=owner_mode,
            queue_mode=queue_mode,
            handoff_mode=handoff_mode,
            local_budget=local_budget,
            replay_budget=replay_budget,
            dirty_min_degree=dirty_min_degree,
        )


def print_cta_worklist_gate_for_cta_mode(rows: Sequence[Dict[str, str]],
                                         limit: int,
                                         baseline_mode: Mode,
                                         regression_threshold: float,
                                         min_runs: int,
                                         owner_mode: str,
                                         queue_mode: str,
                                         handoff_mode: str,
                                         local_budget: str,
                                         replay_budget: str,
                                         dirty_min_degree: str) -> None:
    ok = ok_rows(rows)
    cta_mode: Mode = ("shared", "cta_worklist")
    old_worklist_modes = [("shared", "worklist"), ("private", "worklist")]

    by_input: Dict[str, List[Dict[str, str]]] = defaultdict(list)
    for row in ok:
        by_input[get(row, "input")].append(row)

    cta_vs_baseline: List[Tuple[float, str, Dict[str, str]]] = []
    cta_vs_worklist: List[Tuple[float, str, Dict[str, str]]] = []
    host_round_ratios: List[float] = []
    cta_cpu_ratios: List[float] = []
    baseline_cpu_ratios: List[float] = []
    cta_overflows: List[float] = []
    queue_overflows: List[float] = []
    budget_spills: List[float] = []
    seed_overflows: List[float] = []
    cta_pushes: List[float] = []
    cta_cross: List[float] = []
    owner_map_ms: List[float] = []
    owner_balance: List[float] = []
    owner_weight_balance: List[float] = []
    owner_local: List[float] = []
    owner_cross: List[float] = []
    seed_nonempty: List[float] = []
    seed_empty: List[float] = []
    seed_max_load: List[float] = []
    seed_balance: List[float] = []
    replay_rounds: List[float] = []
    replay_drains: List[float] = []
    replay_spills: List[float] = []
    dirty_vars: List[float] = []
    dirty_scans: List[float] = []
    dirty_hits: List[float] = []
    cross_push_avoided: List[float] = []
    dirty_fallback_push: List[float] = []

    for input_path, subset in by_input.items():
        grouped: Dict[Mode, List[Dict[str, str]]] = defaultdict(list)
        for row in subset:
            grouped[mode_of(row)].append(row)

        cta_rows = [
            row for row in grouped.get(cta_mode, [])
            if (
                cta_owner_mode(row) == owner_mode and
                cta_queue_mode(row) == queue_mode and
                cta_handoff_mode(row) == handoff_mode and
                cta_local_round_budget(row) == local_budget and
                cta_replay_round_budget(row) == replay_budget and
                cta_dirty_pull_min_degree(row) == dirty_min_degree
            )
        ]
        if len(cta_rows) < min_runs:
            continue

        cta_solve = mean_metric(cta_rows, metric_ms)
        cta_rounds = mean_metric(cta_rows, round_count)
        cta_sample = cta_rows[0]
        cta_overflows.extend(as_float(row, "cta_overflow_count") for row in cta_rows)
        for row in cta_rows:
            if (
                get(row, "cta_queue_overflow_count") or
                get(row, "cta_budget_spill_count") or
                get(row, "cta_seed_overflow_count")
            ):
                queue_overflows.append(as_float(row, "cta_queue_overflow_count"))
                budget_spills.append(as_float(row, "cta_budget_spill_count"))
                seed_overflows.append(as_float(row, "cta_seed_overflow_count"))
            else:
                queue_overflows.append(as_float(row, "cta_overflow_count"))
        cta_pushes.extend(as_float(row, "cta_queue_push_count") for row in cta_rows)
        cta_cross.extend(as_float(row, "cta_cross_push_count") for row in cta_rows)
        dirty_vars.extend(as_float(row, "dirty_var_count") for row in cta_rows)
        dirty_scans.extend(as_float(row, "dirty_pull_scan_count") for row in cta_rows)
        dirty_hits.extend(as_float(row, "dirty_pull_hit_count") for row in cta_rows)
        cross_push_avoided.extend(
            as_float(row, "cross_push_avoided_count") for row in cta_rows
        )
        dirty_fallback_push.extend(
            as_float(row, "dirty_pull_fallback_push_count") for row in cta_rows
        )
        owner_map_ms.extend(as_float(row, "owner_map_build_ms") for row in cta_rows)
        owner_balance.extend(as_float(row, "owner_balance_p95") for row in cta_rows)
        owner_weight_balance.extend(
            as_float(row, "owner_weight_balance_p95") for row in cta_rows
        )
        owner_local.extend(as_float(row, "owner_local_push_count") for row in cta_rows)
        owner_cross.extend(as_float(row, "owner_cross_push_count") for row in cta_rows)
        seed_nonempty.extend(
            as_float(row, "seed_owner_nonempty_count") for row in cta_rows
        )
        seed_empty.extend(as_float(row, "seed_empty_owner_count") for row in cta_rows)
        seed_max_load.extend(as_float(row, "seed_max_owner_load") for row in cta_rows)
        seed_balance.extend(as_float(row, "seed_owner_balance_p95") for row in cta_rows)
        replay_rounds.extend(
            as_float(row, "cta_budget_replay_rounds") for row in cta_rows
        )
        replay_drains.extend(
            as_float(row, "cta_budget_replay_drain_count") for row in cta_rows
        )
        replay_spills.extend(
            as_float(row, "cta_budget_replay_spill_count") for row in cta_rows
        )
        cta_cpu_ratios.extend(
            metal_cpu_ratio(row) for row in cta_rows if metal_cpu_ratio(row) > 0.0
        )

        baseline_rows = grouped.get(baseline_mode, [])
        if len(baseline_rows) >= min_runs:
            baseline_solve = mean_metric(baseline_rows, metric_ms)
            if baseline_solve > 0.0:
                cta_vs_baseline.append(
                    (cta_solve / baseline_solve, input_path, cta_sample)
                )
            baseline_rounds = mean_metric(baseline_rows, round_count)
            if baseline_rounds > 0.0:
                host_round_ratios.append(cta_rounds / baseline_rounds)
            baseline_cpu_ratios.extend(
                metal_cpu_ratio(row)
                for row in baseline_rows
                if metal_cpu_ratio(row) > 0.0
            )

        old_candidates = []
        for mode in old_worklist_modes:
            candidate_rows = grouped.get(mode, [])
            if len(candidate_rows) >= min_runs:
                old_candidates.append(mean_metric(candidate_rows, metric_ms))
        if old_candidates:
            best_old = min(value for value in old_candidates if value > 0.0)
            if best_old > 0.0:
                cta_vs_worklist.append((cta_solve / best_old, input_path, cta_sample))

    if not cta_vs_baseline and not cta_vs_worklist:
        return

    baseline_ratios = [ratio for ratio, _, _ in cta_vs_baseline]
    worklist_ratios = [ratio for ratio, _, _ in cta_vs_worklist]
    overflow_positive = sum(1 for value in cta_overflows if value > 0.0)
    queue_overflow_positive = sum(1 for value in queue_overflows if value > 0.0)
    seed_overflow_positive = sum(1 for value in seed_overflows if value > 0.0)
    cta_cpu_p50 = percentile(cta_cpu_ratios, 50) if cta_cpu_ratios else 0.0
    cta_cpu_p95 = percentile(cta_cpu_ratios, 95) if cta_cpu_ratios else 0.0
    base_cpu_p50 = percentile(baseline_cpu_ratios, 50) if baseline_cpu_ratios else 0.0
    base_cpu_p95 = percentile(baseline_cpu_ratios, 95) if baseline_cpu_ratios else 0.0
    p95_ok = bool(baseline_ratios) and percentile(baseline_ratios, 95) <= regression_threshold
    worklist_ok = bool(worklist_ratios) and percentile(worklist_ratios, 95) <= 1.0
    host_round_ok = bool(host_round_ratios) and percentile(host_round_ratios, 50) < 1.0
    queue_overflow_ok = (
        not queue_overflows or percentile(queue_overflows, 95) == 0.0
    )
    seed_overflow_ok = (
        not seed_overflows or percentile(seed_overflows, 95) == 0.0
    )
    overflow_ok = queue_overflow_ok and seed_overflow_ok
    cpu_ratio_ok = (
        bool(cta_cpu_ratios)
        and bool(baseline_cpu_ratios)
        and (cta_cpu_p50 < base_cpu_p50 or cta_cpu_p95 < base_cpu_p95)
    )
    eligible = p95_ok and worklist_ok and host_round_ok and overflow_ok and cpu_ratio_ok

    print(
        f"\n[cta worklist gate owner_mode={owner_mode} queue_mode={queue_mode} "
        f"handoff_mode={handoff_mode} "
        f"local_budget={local_budget} replay_budget={replay_budget} "
        f"dirty_min_degree={dirty_min_degree}]"
    )
    if baseline_ratios:
        print(
            "cta_vs_{baseline} compared={count} p50={p50:.2f}x "
            "p95={p95:.2f}x p99={p99:.2f}x pass_p95={passed}".format(
                baseline=f"{baseline_mode[0]}+{baseline_mode[1]}",
                count=len(baseline_ratios),
                p50=percentile(baseline_ratios, 50),
                p95=percentile(baseline_ratios, 95),
                p99=percentile(baseline_ratios, 99),
                passed=str(p95_ok).lower(),
            )
        )
    if (
        dirty_vars or dirty_scans or dirty_hits or cross_push_avoided or
        dirty_fallback_push
    ):
        print(
            "dirty_pull_stats dirty_vars_p50={vars_p50:.2f} "
            "scan_p50={scan_p50:.2f} hit_p50={hit_p50:.2f} "
            "cross_push_avoided_p50={avoided_p50:.2f} "
            "fallback_push_p50={fallback_p50:.2f}".format(
                vars_p50=percentile(dirty_vars, 50) if dirty_vars else 0.0,
                scan_p50=percentile(dirty_scans, 50) if dirty_scans else 0.0,
                hit_p50=percentile(dirty_hits, 50) if dirty_hits else 0.0,
                avoided_p50=percentile(cross_push_avoided, 50)
                if cross_push_avoided
                else 0.0,
                fallback_p50=percentile(dirty_fallback_push, 50)
                if dirty_fallback_push
                else 0.0,
            )
        )
    if worklist_ratios:
        print(
            "cta_vs_best_worklist compared={count} p50={p50:.2f}x "
            "p95={p95:.2f}x pass_p95={passed}".format(
                count=len(worklist_ratios),
                p50=percentile(worklist_ratios, 50),
                p95=percentile(worklist_ratios, 95),
                passed=str(worklist_ok).lower(),
            )
        )
    if host_round_ratios:
        print(
            "host_round_ratio_vs_baseline p50={p50:.2f}x p95={p95:.2f}x "
            "reduced_p50={passed}".format(
                p50=percentile(host_round_ratios, 50),
                p95=percentile(host_round_ratios, 95),
                passed=str(host_round_ok).lower(),
            )
        )
    if cta_overflows:
        print(
            "cta_stats overflow_p95={overflow_p95:.2f} overflow_max={overflow_max:.2f} "
            "overflow_rows_gt0={overflow_rows} queue_overflow_p95={q_p95:.2f} "
            "seed_overflow_p95={seed_p95:.2f} budget_spill_p95={spill_p95:.2f} "
            "queue_overflow_rows_gt0={q_rows} seed_overflow_rows_gt0={seed_rows} "
            "queue_push_p50={push_p50:.2f} cross_push_p50={cross_p50:.2f}".format(
                overflow_p95=percentile(cta_overflows, 95),
                overflow_max=max(cta_overflows),
                overflow_rows=overflow_positive,
                q_p95=percentile(queue_overflows, 95)
                if queue_overflows
                else 0.0,
                seed_p95=percentile(seed_overflows, 95)
                if seed_overflows
                else 0.0,
                spill_p95=percentile(budget_spills, 95)
                if budget_spills
                else 0.0,
                q_rows=queue_overflow_positive,
                seed_rows=seed_overflow_positive,
                push_p50=percentile(cta_pushes, 50) if cta_pushes else 0.0,
                cross_p50=percentile(cta_cross, 50) if cta_cross else 0.0,
            )
        )
    if seed_nonempty or seed_empty or seed_max_load or seed_balance:
        print(
            "seed_stats nonempty_avg={nonempty:.2f} empty_avg={empty:.2f} "
            "max_load_p95={max_load:.2f} balance_p95_avg={balance:.2f}".format(
                nonempty=statistics.mean(seed_nonempty)
                if any(seed_nonempty)
                else 0.0,
                empty=statistics.mean(seed_empty) if any(seed_empty) else 0.0,
                max_load=percentile(seed_max_load, 95)
                if seed_max_load
                else 0.0,
                balance=statistics.mean(seed_balance)
                if any(seed_balance)
                else 0.0,
            )
        )
    if replay_rounds or replay_drains or replay_spills:
        print(
            "replay_stats rounds_p50={rounds_p50:.2f} drain_p95={drain_p95:.2f} "
            "spill_p95={spill_p95:.2f}".format(
                rounds_p50=percentile(replay_rounds, 50)
                if replay_rounds
                else 0.0,
                drain_p95=percentile(replay_drains, 95)
                if replay_drains
                else 0.0,
                spill_p95=percentile(replay_spills, 95)
                if replay_spills
                else 0.0,
            )
        )
    if (
        owner_map_ms or owner_balance or owner_weight_balance or
        owner_local or owner_cross
    ):
        print(
            "owner_stats map_build_ms_avg={map_ms:.3f} "
            "balance_p95_avg={balance:.2f} weight_balance_p95_avg={w_balance:.2f} "
            "owner_local_p50={local:.2f} owner_cross_p50={cross:.2f}".format(
                map_ms=statistics.mean(owner_map_ms) if any(owner_map_ms) else 0.0,
                balance=statistics.mean(owner_balance)
                if any(owner_balance)
                else 0.0,
                w_balance=statistics.mean(owner_weight_balance)
                if any(owner_weight_balance)
                else 0.0,
                local=percentile(owner_local, 50) if owner_local else 0.0,
                cross=percentile(owner_cross, 50) if owner_cross else 0.0,
            )
        )
    if cta_cpu_ratios:
        print(
            "metal_cpu_ratio cta_p50={cta_p50:.2f}x cta_p95={cta_p95:.2f}x "
            "baseline_p50={base_p50:.2f}x baseline_p95={base_p95:.2f}x "
            "improved={improved}".format(
                cta_p50=cta_cpu_p50,
                cta_p95=cta_cpu_p95,
                base_p50=base_cpu_p50,
                base_p95=base_cpu_p95,
                improved=str(cpu_ratio_ok).lower(),
            )
        )
    reason = []
    if not p95_ok:
        reason.append("baseline_p95_regression")
    if not worklist_ok:
        reason.append("worklist_p95_regression")
    if not host_round_ok:
        reason.append("host_round_not_reduced")
    if not overflow_ok:
        reason.append("cta_queue_or_seed_overflow")
    if not cpu_ratio_ok:
        reason.append("metal_cpu_ratio_not_improved")
    print(
        "decision={decision} reason={reason}".format(
            decision="eligible" if eligible else "report_only",
            reason="none" if not reason else ",".join(reason),
        )
    )

    regressions = [
        (ratio, input_path, row)
        for ratio, input_path, row in cta_vs_baseline
        if ratio > regression_threshold
    ]
    if regressions:
        print("[cta regressions > threshold]")
        for ratio, input_path, row in sorted(regressions, reverse=True)[:limit]:
            print(
                "  {ratio:.2f}x input={input} cons={cons} dom={dom} bitw={bitw} "
                "rounds={rounds} overflow={overflow} path={path}".format(
                    ratio=ratio,
                    input=input_path,
                    cons=get(row, "num_constraints"),
                    dom=get(row, "max_dom_size"),
                    bitw=get(row, "bit_words"),
                    rounds=get(row, "host_round_count"),
                    overflow=get(row, "cta_overflow_count"),
                    path=path_label(row),
                )
            )


def int_field(row: Dict[str, str], key: str) -> int:
    try:
        return int(float(get(row, key)))
    except ValueError:
        return 0


def is_large_any(row: Dict[str, str]) -> bool:
    return (
        int_field(row, "num_constraints") >= 256 or
        int_field(row, "max_dom_size") >= 64 or
        int_field(row, "bit_words") >= 2
    )


def print_bulk_sync_mask_gate(rows: Sequence[Dict[str, str]],
                              limit: int,
                              baseline_mode: Mode,
                              regression_threshold: float,
                              min_runs: int) -> None:
    ok = ok_rows(rows)
    bulk_mode: Mode = ("shared", "bulk_sync_mask")
    if not any(mode_of(row) == bulk_mode for row in ok):
        return

    by_input: Dict[str, List[Dict[str, str]]] = defaultdict(list)
    for row in ok:
        by_input[get(row, "input")].append(row)

    all_ratios: List[Tuple[float, str, Dict[str, str]]] = []
    large_any_ratios: List[Tuple[float, str, Dict[str, str]]] = []
    large_prop_ratios: List[Tuple[float, str, Dict[str, str]]] = []
    dispatch_ratios: List[float] = []
    proposed: List[float] = []
    actual: List[float] = []
    changed_words: List[float] = []
    frontier_pushes: List[float] = []
    rounds: List[float] = []
    actual_mismatches = 0

    for input_path, subset in by_input.items():
        grouped: Dict[Mode, List[Dict[str, str]]] = defaultdict(list)
        for row in subset:
            grouped[mode_of(row)].append(row)
        baseline_rows = grouped.get(baseline_mode, [])
        bulk_rows = grouped.get(bulk_mode, [])
        if len(baseline_rows) < min_runs or len(bulk_rows) < min_runs:
            continue

        baseline_solve = statistics.mean(metric_ms(row) for row in baseline_rows)
        bulk_solve = statistics.mean(metric_ms(row) for row in bulk_rows)
        if baseline_solve <= 0.0 or bulk_solve <= 0.0:
            continue
        sample = bulk_rows[0]
        ratio_entry = (bulk_solve / baseline_solve, input_path, sample)
        all_ratios.append(ratio_entry)

        baseline_dispatch = statistics.mean(
            as_float(row, "dispatch_count") for row in baseline_rows
        )
        bulk_dispatch = statistics.mean(
            as_float(row, "dispatch_count") for row in bulk_rows
        )
        if baseline_dispatch > 0.0:
            dispatch_ratios.append(bulk_dispatch / baseline_dispatch)

        proposed.extend(
            as_float(row, "bulk_mask_proposed_deletion_count") for row in bulk_rows
        )
        actual.extend(
            as_float(row, "bulk_mask_actual_deletion_count") for row in bulk_rows
        )
        changed_words.extend(
            as_float(row, "bulk_mask_changed_word_count") for row in bulk_rows
        )
        frontier_pushes.extend(
            as_float(row, "bulk_mask_frontier_push_count") for row in bulk_rows
        )
        rounds.extend(as_float(row, "bulk_mask_rounds") for row in bulk_rows)
        for row in bulk_rows:
            bulk_actual = as_float(row, "bulk_mask_actual_deletion_count")
            deletions = as_float(row, "deletions")
            if bulk_actual > 0.0 and deletions > 0.0 and bulk_actual != deletions:
                actual_mismatches += 1

        large_sample = baseline_rows[0]
        if is_large_any(large_sample):
            large_any_ratios.append(ratio_entry)
            baseline_deletions_p50 = percentile(
                [as_float(row, "deletions") for row in baseline_rows], 50
            )
            baseline_rounds_p50 = percentile(
                [round_count(row) for row in baseline_rows], 50
            )
            baseline_solve_p50 = percentile(
                [metric_ms(row) for row in baseline_rows], 50
            )
            if (
                baseline_deletions_p50 >= 64.0 or
                baseline_rounds_p50 >= 2.0 or
                baseline_solve_p50 >= 1.0
            ):
                large_prop_ratios.append(ratio_entry)

    if not all_ratios:
        return

    def print_ratio_line(label: str,
                         values: Sequence[Tuple[float, str, Dict[str, str]]],
                         promote_threshold: float) -> None:
        if not values:
            print(f"{label} compared=0")
            return
        ratios = [ratio for ratio, _, _ in values]
        regressions = sum(1 for ratio in ratios if ratio > regression_threshold)
        promote = (
            percentile(ratios, 50) <= promote_threshold and
            percentile(ratios, 95) <= promote_threshold and
            regressions <= max(1, len(ratios) // 10)
        )
        print(
            "{label} compared={count} p50={p50:.2f}x p95={p95:.2f}x "
            "regressions_gt_threshold={reg} promote_signal={promote}".format(
                label=label,
                count=len(ratios),
                p50=percentile(ratios, 50),
                p95=percentile(ratios, 95),
                reg=regressions,
                promote=str(promote).lower(),
            )
        )

    print("\n[bulk sync mask gate]")
    print_ratio_line("bulk_vs_{0}+{1}".format(*baseline_mode), all_ratios, 1.05)
    print_ratio_line("large_any", large_any_ratios, 0.90)
    print_ratio_line("large_prop", large_prop_ratios, 0.90)
    if dispatch_ratios:
        print(
            "dispatch_ratio_vs_baseline p50={p50:.2f}x p95={p95:.2f}x".format(
                p50=percentile(dispatch_ratios, 50),
                p95=percentile(dispatch_ratios, 95),
            )
        )
    print(
        "bulk_stats proposed_p50={proposed:.2f} actual_p50={actual:.2f} "
        "changed_words_p50={changed:.2f} frontier_push_p50={push:.2f} "
        "rounds_p50={rounds:.2f} actual_deletion_mismatch_rows={mismatch}".format(
            proposed=percentile(proposed, 50) if proposed else 0.0,
            actual=percentile(actual, 50) if actual else 0.0,
            changed=percentile(changed_words, 50) if changed_words else 0.0,
            push=percentile(frontier_pushes, 50) if frontier_pushes else 0.0,
            rounds=percentile(rounds, 50) if rounds else 0.0,
            mismatch=actual_mismatches,
        )
    )

    regressions = [
        (ratio, input_path, row)
        for ratio, input_path, row in all_ratios
        if ratio > regression_threshold
    ]
    if regressions:
        print("[bulk regressions > threshold]")
        for ratio, input_path, row in sorted(regressions, reverse=True)[:limit]:
            print(
                "  {ratio:.2f}x input={input} cons={cons} dom={dom} bitw={bitw} "
                "rounds={rounds} bulk_actual={actual} path={path}".format(
                    ratio=ratio,
                    input=input_path,
                    cons=get(row, "num_constraints"),
                    dom=get(row, "max_dom_size"),
                    bitw=get(row, "bit_words"),
                    rounds=get(row, "host_round_count"),
                    actual=get(row, "bulk_mask_actual_deletion_count"),
                    path=path_label(row),
                )
            )


def print_family_summary(rows: Sequence[Dict[str, str]], limit: int) -> None:
    ok = ok_rows(rows)
    if not ok:
        return
    baseline = [row for row in ok if mode_of(row) == BASELINE]
    if not baseline:
        baseline = ok
    grouped: Dict[str, List[float]] = defaultdict(list)
    for row in baseline:
        grouped[family_name(get(row, "input"))].append(metric_ms(row))
    print("\n[family summary]")
    for family, values in sorted(
        grouped.items(),
        key=lambda item: (-statistics.mean(item[1]), item[0]),
    )[:limit]:
        print(
            f"  {family}: rows={len(values)} avg={statistics.mean(values):.3f} "
            f"p95={percentile(values, 95):.3f}"
        )


def print_slowest(rows: Sequence[Dict[str, str]], limit: int) -> None:
    ok = ok_rows(rows)
    if not ok:
        return
    grouped: Dict[Tuple[str, Mode], List[Dict[str, str]]] = defaultdict(list)
    for row in ok:
        grouped[(get(row, "input"), mode_of(row))].append(row)
    ranked = []
    for (input_path, mode), subset in grouped.items():
        avg = statistics.mean(metric_ms(row) for row in subset)
        ranked.append((avg, input_path, mode, subset[0]))
    print("\n[slowest]")
    for avg, input_path, mode, row in sorted(ranked, reverse=True)[:limit]:
        print(
            "{avg:.3f}ms {mode} {input} vars={vars} cons={cons} "
            "maxdom={maxdom} bitw={bitw} iters={iters} del={delv} inc={inc}".format(
                avg=avg,
                mode=f"{mode[0]}+{mode[1]}",
                input=input_path,
                vars=get(row, "num_vars"),
                cons=get(row, "num_constraints"),
                maxdom=get(row, "max_dom_size"),
                bitw=get(row, "bit_words"),
                iters=get(row, "iterations"),
                delv=get(row, "deletions"),
                inc=get(row, "inconsistent"),
            )
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", nargs="+", type=Path)
    parser.add_argument("--top", type=int, default=10)
    parser.add_argument(
        "--recommend-policy",
        action="store_true",
        help="Print v3 report-only policy recommendations from CSV evidence.",
    )
    parser.add_argument(
        "--baseline-mode",
        type=parse_mode,
        default=BASELINE,
        help="Baseline mode for relative policy decisions, e.g. shared+flags.",
    )
    parser.add_argument(
        "--regression-threshold",
        type=float,
        default=1.05,
        help="Ratio above baseline considered a regression.",
    )
    parser.add_argument(
        "--min-runs",
        type=int,
        default=3,
        help="Minimum rows per input/mode before using it for recommendations.",
    )
    parser.add_argument(
        "--bucket-min-instances",
        type=int,
        default=3,
        help="Minimum instances in a bucket before simulating a bucket policy.",
    )
    args = parser.parse_args()
    if args.min_runs < 1:
        parser.error("--min-runs must be >= 1")
    if args.bucket_min_instances < 1:
        parser.error("--bucket-min-instances must be >= 1")

    all_rows: List[Dict[str, str]] = []
    for path in args.csv:
        rows = read_rows(path)
        print_overview(path, rows)
        print_error_summary(rows, args.top)
        print_mode_summary(rows)
        print_ratio_summary(rows, min(args.top, 5))
        print_metal_vs_cpu(rows, min(args.top, 5))
        print_cta_worklist_gate(
            rows,
            args.top,
            baseline_mode=args.baseline_mode,
            regression_threshold=args.regression_threshold,
            min_runs=args.min_runs,
        )
        print_bulk_sync_mask_gate(
            rows,
            args.top,
            baseline_mode=args.baseline_mode,
            regression_threshold=args.regression_threshold,
            min_runs=args.min_runs,
        )
        print_runtime_policy_summary(
            rows,
            args.top,
            fallback_mode=args.baseline_mode,
            regression_threshold=args.regression_threshold,
            min_runs=args.min_runs,
        )
        print_dispatch_timing_split(rows)
        print_recommended_policy_summary(
            rows,
            args.top,
            baseline_mode=args.baseline_mode,
            regression_threshold=args.regression_threshold,
            min_runs=args.min_runs,
        )
        if args.recommend_policy:
            print_policy_recommendations(
                rows,
                args.top,
                baseline_mode=args.baseline_mode,
                regression_threshold=args.regression_threshold,
                min_runs=args.min_runs,
            )
            print_bucket_policy_simulation(
                rows,
                args.top,
                fallback_mode=args.baseline_mode,
                regression_threshold=args.regression_threshold,
                min_runs=args.min_runs,
                bucket_min_instances=args.bucket_min_instances,
            )
        print_family_summary(rows, args.top)
        print_slowest(rows, args.top)
        all_rows.extend(rows)

    if len(args.csv) > 1:
        print_overview(Path("<combined>"), all_rows)
        print_metal_vs_cpu(all_rows, min(args.top, 5))
        print_cta_worklist_gate(
            all_rows,
            args.top,
            baseline_mode=args.baseline_mode,
            regression_threshold=args.regression_threshold,
            min_runs=args.min_runs,
        )
        print_bulk_sync_mask_gate(
            all_rows,
            args.top,
            baseline_mode=args.baseline_mode,
            regression_threshold=args.regression_threshold,
            min_runs=args.min_runs,
        )
        print_runtime_policy_summary(
            all_rows,
            args.top,
            fallback_mode=args.baseline_mode,
            regression_threshold=args.regression_threshold,
            min_runs=args.min_runs,
        )
        print_dispatch_timing_split(all_rows)
        print_recommended_policy_summary(
            all_rows,
            args.top,
            baseline_mode=args.baseline_mode,
            regression_threshold=args.regression_threshold,
            min_runs=args.min_runs,
        )
        if args.recommend_policy:
            print_policy_recommendations(
                all_rows,
                args.top,
                baseline_mode=args.baseline_mode,
                regression_threshold=args.regression_threshold,
                min_runs=args.min_runs,
            )
            print_bucket_policy_simulation(
                all_rows,
                args.top,
                fallback_mode=args.baseline_mode,
                regression_threshold=args.regression_threshold,
                min_runs=args.min_runs,
                bucket_min_instances=args.bucket_min_instances,
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
