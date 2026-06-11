#!/usr/bin/env python3
import argparse
import json
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/fpga_cpim/fpga_cpim_sim")
    parser.add_argument("--seeds", type=int, default=100)
    parser.add_argument("--vars", type=int, default=8)
    parser.add_argument("--domain", type=int, default=16)
    parser.add_argument("--density", type=float, default=0.3)
    parser.add_argument("--mode", default="nsacq")
    args = parser.parse_args()

    binary = Path(args.binary)
    failures = 0
    for seed in range(args.seeds):
        cmd = [
            str(binary),
            "--instance",
            "synthetic",
            "--vars",
            str(args.vars),
            "--domain",
            str(args.domain),
            "--density",
            str(args.density),
            "--seed",
            str(seed),
            "--mode",
            args.mode,
            "--max-events",
            "1000000",
            "--max-revise",
            "1000000",
            "--max-epochs",
            "10000",
        ]
        proc = subprocess.run(cmd, text=True, capture_output=True, check=False)
        if proc.returncode != 0:
            failures += 1
            print(f"seed={seed} failed: {proc.stderr.strip()}")
            continue
        data = json.loads(proc.stdout)
        if data["results"]["unknown"] != 0:
            failures += 1
            print(f"seed={seed} unexpected UNKNOWN in high-budget run")

    if failures:
        raise SystemExit(f"{failures} sweep cases failed")
    print(f"ok: {args.seeds} seeds")


if __name__ == "__main__":
    main()
