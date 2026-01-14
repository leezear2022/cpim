#!/usr/bin/env python3
"""Compare AC3bit vs MSAC3bit (SAC1/SAC3) performance.

Usage:
    python3 compare_sac_algorithms.py [--instances PATH] [--timeout SECONDS]
"""

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


@dataclass
class RunResult:
    """Result of a single run."""
    algorithm: str
    instance: str
    time_ms: float
    positives: int
    negatives: int
    solution: Optional[str]
    timeout: bool
    # MSAC specific
    probes: int = 0
    probe_failures: int = 0
    values_removed: int = 0
    probe_time_ms: float = 0.0


def run_solver(binary: str, instance: str, algorithm: str, msac_mode: str = None,
               timeout_s: int = 60) -> RunResult:
    """Run solver and parse results."""
    cmd = [binary, f"--bench_path={instance}", f"--ac_algorithm={algorithm}"]
    if msac_mode:
        cmd.append(f"--msac_mode={msac_mode}")
        cmd.append("--msac_verbose_stats")

    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout_s
        )
        output = result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return RunResult(
            algorithm=f"{algorithm}({msac_mode})" if msac_mode else algorithm,
            instance=os.path.basename(instance),
            time_ms=timeout_s * 1000,
            positives=0, negatives=0, solution=None, timeout=True
        )

    # Parse MAC stats
    mac_match = re.search(
        r"MAC stats: time=(\d+) ms, positives=(\d+), negatives=(\d+)", output
    )
    if mac_match:
        time_ms = float(mac_match.group(1))
        positives = int(mac_match.group(2))
        negatives = int(mac_match.group(3))
    else:
        time_ms, positives, negatives = 0, 0, 0

    # Parse solution
    sol_match = re.search(r"MAC solution \(canonical indices\): (.+)", output)
    solution = sol_match.group(1) if sol_match else None

    # Parse MSAC stats
    probes, probe_failures, values_removed, probe_time_ms = 0, 0, 0, 0.0
    if msac_mode:
        probes_match = re.search(r"Total probes: (\d+) \((\d+) failed\)", output)
        if probes_match:
            probes = int(probes_match.group(1))
            probe_failures = int(probes_match.group(2))

        removed_match = re.search(r"Values removed: (\d+)", output)
        if removed_match:
            values_removed = int(removed_match.group(1))

        probe_time_match = re.search(r"Probe time: ([\d.]+) ms", output)
        if probe_time_match:
            probe_time_ms = float(probe_time_match.group(1))

    return RunResult(
        algorithm=f"{algorithm}({msac_mode})" if msac_mode else algorithm,
        instance=os.path.basename(instance),
        time_ms=time_ms,
        positives=positives,
        negatives=negatives,
        solution=solution,
        timeout=False,
        probes=probes,
        probe_failures=probe_failures,
        values_removed=values_removed,
        probe_time_ms=probe_time_ms
    )


def main():
    parser = argparse.ArgumentParser(description="Compare SAC algorithms")
    parser.add_argument(
        "--instances", type=str, default="../tests/data/bench",
        help="Path to instance directory or single file"
    )
    parser.add_argument("--timeout", type=int, default=60, help="Timeout in seconds")
    parser.add_argument("--binary", type=str, default="./cpim_test_parser",
                        help="Path to solver binary")
    args = parser.parse_args()

    # Find instances
    instances_path = Path(args.instances)
    if instances_path.is_file():
        instances = [str(instances_path)]
    else:
        instances = sorted(str(p) for p in instances_path.glob("*.xml"))

    if not instances:
        print(f"No XML files found in {args.instances}")
        sys.exit(1)

    # Run comparisons
    algorithms = [
        ("AC3bit", None),
        ("MSAC3bit", "SAC1"),
        ("MSAC3bit", "SAC3"),
        ("MSAC3bit", "SAC_SDS"),
    ]

    print(f"{'Instance':<35} {'Algorithm':<18} {'Time(ms)':<10} {'P/N':<12} "
          f"{'Probes':<10} {'Removed':<8} {'Solution':<10}")
    print("=" * 120)

    for instance in instances:
        results = []
        for alg, mode in algorithms:
            result = run_solver(args.binary, instance, alg, mode, args.timeout)
            results.append(result)

            if result.timeout:
                status = "TIMEOUT"
            elif result.solution:
                status = "SAT"
            else:
                status = "UNSAT?"

            probes_str = f"{result.probes}({result.probe_failures})" if result.probes else "-"
            removed_str = str(result.values_removed) if result.values_removed else "-"

            print(f"{result.instance:<35} {result.algorithm:<18} {result.time_ms:<10.1f} "
                  f"{result.positives}/{result.negatives:<10} {probes_str:<10} {removed_str:<8} {status:<10}")

        # Verify solutions match
        solutions = [r.solution for r in results if r.solution]
        if len(set(solutions)) > 1:
            print(f"  WARNING: Solutions differ!")

        print("-" * 120)


if __name__ == "__main__":
    main()
