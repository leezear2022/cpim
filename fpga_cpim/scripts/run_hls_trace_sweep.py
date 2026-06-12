#!/usr/bin/env python3
import argparse
import io
import os
import subprocess
import sys
from pathlib import Path

import parse_hls_trace


def default_repo_root():
    return Path(__file__).resolve().parents[2]


def default_cxx():
    return os.environ.get("CXX", "g++")


def resolve_path(repo_root, path):
    candidate = Path(path)
    if candidate.is_absolute():
        return candidate
    return repo_root / candidate


def profile_macro(profile):
    macros = {
        "stress128": "FPGA_CPIM_HLS_PROFILE_STRESS128",
        "z7020_small": "FPGA_CPIM_HLS_PROFILE_Z7020_SMALL",
        "z7020_probe2": "FPGA_CPIM_HLS_PROFILE_Z7020_PROBE2",
    }
    if profile not in macros:
        raise ValueError(f"unknown profile: {profile}")
    return macros[profile]


def compile_hls(repo_root, binary, cxx, profile):
    hls_dir = repo_root / "fpga_cpim" / "hls"
    sources = sorted(str(path) for path in hls_dir.glob("*.cpp"))
    binary.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        cxx,
        "-std=c++17",
        "-I",
        str(hls_dir),
        f"-DFPGA_CPIM_HLS_PROFILE={profile_macro(profile)}",
    ] + sources + ["-o", str(binary)]
    subprocess.run(cmd, cwd=repo_root, check=True)
    return cmd


def run_hls(binary, tiles, capacities, fixtures, profile):
    cmd = [
        str(binary),
        "--pressure-only",
        f"--profile={profile}",
        f"--tiles={tiles}",
        f"--capacity-sweep={capacities}",
        f"--fixtures={fixtures}",
    ]
    result = subprocess.run(
        cmd,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return cmd, result.stdout, result.stderr


def write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def write_jsonl(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as stream:
        parse_hls_trace.write_jsonl(rows, stream)


def print_summary(rows):
    print(
        "kind profile tiles capacity status events epochs queuePeakTotal "
        "queuePeakPart semanticMin chosenDepth routerOverflow"
    )
    for row in rows:
        print(
            row["kind"],
            row["profile"],
            row["tiles"],
            row["capacity"],
            row["status"],
            row["events"],
            row["epochs"],
            row["queue_peak_total"],
            row["queue_peak_partition"],
            row["semantic_min_capacity"],
            row["chosen_depth"],
            row["router_overflow"],
        )


def print_capacity_threshold(rows):
    keys = sorted(
        {
            (row.get("profile", ""), row.get("graph", ""))
            for row in rows
            if row["kind"] == "capacity"
        }
    )
    for profile, graph in keys:
        graph_rows = [
            row
            for row in rows
            if row["kind"] == "capacity"
            and row.get("profile") == profile
            and row.get("graph") == graph
        ]
        unknown_caps = [
            row["capacity"] for row in graph_rows if row["status"] == "UNKNOWN"
        ]
        ok_caps = [row["capacity"] for row in graph_rows if row["status"] == "OK"]
        if unknown_caps and ok_caps:
            print(
                "capacity_threshold "
                f"profile={profile} graph={graph} "
                f"max_unknown={max(unknown_caps)} min_ok={min(ok_caps)}"
            )
        elif ok_caps:
            print(
                "capacity_threshold "
                f"profile={profile} graph={graph} "
                "semantic_min<=min_tested_capacity "
                f"min_tested_capacity={min(ok_caps)}"
            )
        elif unknown_caps:
            print(
                "capacity_threshold "
                f"profile={profile} graph={graph} "
                f"all_unknown max_unknown={max(unknown_caps)}"
            )


def main():
    parser = argparse.ArgumentParser(
        description="Compile hls_tb, run pressure/capacity sweep, emit JSONL."
    )
    parser.add_argument("--repo-root", default=str(default_repo_root()))
    parser.add_argument("--cxx", default=default_cxx())
    parser.add_argument(
        "--binary",
        default=None,
        help="compiled testbench path; defaults to a profile-specific path",
    )
    parser.add_argument("--tiles", default="1,2,4")
    parser.add_argument("--capacity-sweep", default="272,273,274,320,384")
    parser.add_argument("--fixtures", default="chain,random,hub")
    parser.add_argument(
        "--profile",
        choices=["stress128", "z7020_small", "z7020_probe2"],
        default="stress128",
    )
    parser.add_argument("--jsonl", default="build/fpga_cpim/hls_trace_sweep.jsonl")
    parser.add_argument("--raw", default="build/fpga_cpim/hls_trace_sweep.out")
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    binary_name = args.binary or f"build/fpga_cpim/hls_tb_trace_{args.profile}"
    binary = resolve_path(repo_root, binary_name)
    jsonl = resolve_path(repo_root, args.jsonl)
    raw = resolve_path(repo_root, args.raw)

    if not args.no_build:
        compile_cmd = compile_hls(repo_root, binary, args.cxx, args.profile)
        print("compile:", " ".join(compile_cmd))

    run_cmd, stdout, stderr = run_hls(
        binary, args.tiles, args.capacity_sweep, args.fixtures, args.profile
    )
    print("run:", " ".join(run_cmd))
    if stderr:
        sys.stderr.write(stderr)

    rows = parse_hls_trace.enrich_sizing(
        parse_hls_trace.read_rows(io.StringIO(stdout), "all")
    )
    write_text(raw, stdout)
    write_jsonl(jsonl, rows)
    print_summary(rows)
    print_capacity_threshold(rows)
    print(f"wrote_jsonl {jsonl}")
    print(f"wrote_raw {raw}")


if __name__ == "__main__":
    main()
