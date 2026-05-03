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
        "kernel dispatch_count frontier_density verified runners variants "
        "bitsup effective reset worklist_rounds worklist_pushes"
    )
    known_modes = sorted({mode_of(row) for row in ok})
    modes = [mode for mode in MODE_ORDER if mode in known_modes]
    modes.extend(mode for mode in known_modes if mode not in modes)
    for mode in modes:
        subset = [row for row in ok if mode_of(row) == mode]
        elapsed = [metric_ms(row) for row in subset]
        setup = [as_float(row, "setup_ms") for row in subset]
        prepare = [as_float(row, "prepare_ms") for row in subset]
        reset = [as_float(row, "reset_ms") for row in subset]
        dispatch = [as_float(row, "dispatch_ms") for row in subset]
        kernel = [as_float(row, "kernel_ms") for row in subset]
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
        worklist_rounds = [as_float(row, "worklist_rounds") for row in subset]
        worklist_pushes = [as_float(row, "worklist_push_count") for row in subset]
        print(
            "{mode} {rows} {inst} {avg:.3f} {p50:.3f} {p95:.3f} {p99:.3f} "
            "{setup:.3f} {prepare:.3f} {reset:.3f} {dispatch:.3f} "
            "{kernel:.3f} {dc:.2f} {density:.3f} {verified} {runners} "
            "{variants} {bitsup} {effective} {reset_modes} {wl_rounds:.2f} "
            "{wl_pushes:.2f}".format(
                mode=f"{mode[0]}+{mode[1]}",
                rows=len(subset),
                inst=len({get(row, "input") for row in subset}),
                avg=statistics.mean(elapsed),
                p50=percentile(elapsed, 50),
                p95=percentile(elapsed, 95),
                p99=percentile(elapsed, 99),
                setup=statistics.mean(setup),
                prepare=statistics.mean(prepare),
                reset=statistics.mean(reset),
                dispatch=statistics.mean(dispatch),
                kernel=statistics.mean(kernel),
                dc=statistics.mean(dispatch_counts) if any(dispatch_counts) else 0.0,
                density=statistics.mean(frontier_density)
                if any(frontier_density)
                else 0.0,
                verified="/".join(verified),
                runners="/".join(runners),
                variants="/".join(variants),
                bitsup="/".join(bitsup),
                effective="/".join(effective),
                reset_modes="/".join(reset_modes),
                wl_rounds=statistics.mean(worklist_rounds)
                if any(worklist_rounds)
                else 0.0,
                wl_pushes=statistics.mean(worklist_pushes)
                if any(worklist_pushes)
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


def print_recommended_policy_summary(rows: Sequence[Dict[str, str]],
                                     limit: int) -> None:
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
            if not mode_rows:
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

        baseline = per_mode.get(BASELINE)
        auto_mode = ("shared", "auto")
        auto_value = per_mode.get(auto_mode)
        if baseline is None or auto_value is None or baseline <= 0.0:
            continue
        compared += 1
        auto_hits += 1
        if auto_value <= baseline:
            auto_wins += 1
        elif auto_value > baseline * 1.05:
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
            "auto_vs_shared+flags compared={compared} hits={hits} "
            "wins={wins} regressions_gt_5pct={reg}".format(
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
    args = parser.parse_args()

    all_rows: List[Dict[str, str]] = []
    for path in args.csv:
        rows = read_rows(path)
        print_overview(path, rows)
        print_error_summary(rows, args.top)
        print_mode_summary(rows)
        print_ratio_summary(rows, min(args.top, 5))
        print_recommended_policy_summary(rows, args.top)
        print_family_summary(rows, args.top)
        print_slowest(rows, args.top)
        all_rows.extend(rows)

    if len(args.csv) > 1:
        print_overview(Path("<combined>"), all_rows)
        print_recommended_policy_summary(all_rows, args.top)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
