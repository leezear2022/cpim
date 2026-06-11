#!/usr/bin/env python3
import argparse
import json
import subprocess
from pathlib import Path


def run_case(binary, graph, seed, args):
    cmd = [
        str(binary),
        "--instance",
        f"synthetic:{graph}",
        "--vars",
        str(args.vars),
        "--domain",
        str(args.domain),
        "--density",
        str(args.density),
        "--tightness",
        str(args.tightness),
        "--seed",
        str(seed),
        "--mode",
        args.mode,
        "--worlds",
        str(args.worlds),
        "--partitions",
        str(args.partitions),
    ]
    proc = subprocess.run(cmd, text=True, capture_output=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip())
    data = json.loads(proc.stdout)
    return {
        "graph": graph,
        "seed": seed,
        "constraints": data["model"]["num_constraints"],
        "bit_sup_bytes": data["model"]["bit_sup_bytes"],
        "fanout_p95": data["telemetry"]["fanout_p95"],
        "queue_occupancy_p95": data["telemetry"]["queue_occupancy_p95"],
        "queue_occupancy_max": data["telemetry"]["queue_occupancy_max"],
        "cross_event_ratio": data["partition"]["cross_event_ratio"],
        "high_degree_hub_count": data["partition"]["high_degree_hub_count"],
        "bram18_estimate": data["storage"]["bram18_estimate"],
        "uram288_estimate": data["storage"]["uram288_estimate"],
        "unknown_rate": data["results"]["unknown_rate"],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/fpga_cpim/fpga_cpim_sim")
    parser.add_argument("--graphs", default="chain,grid,random,hub")
    parser.add_argument("--seeds", type=int, default=3)
    parser.add_argument("--vars", type=int, default=32)
    parser.add_argument("--domain", type=int, default=32)
    parser.add_argument("--density", type=float, default=0.2)
    parser.add_argument("--tightness", type=float, default=0.5)
    parser.add_argument("--mode", default="nsacq")
    parser.add_argument("--worlds", type=int, default=4)
    parser.add_argument("--partitions", type=int, default=4)
    parser.add_argument("--jsonl", default="")
    args = parser.parse_args()

    binary = Path(args.binary)
    rows = []
    for graph in [g.strip() for g in args.graphs.split(",") if g.strip()]:
        for seed in range(args.seeds):
            rows.append(run_case(binary, graph, seed, args))

    if args.jsonl:
        with open(args.jsonl, "w", encoding="utf-8") as f:
            for row in rows:
                f.write(json.dumps(row, ensure_ascii=False) + "\n")

    by_graph = {}
    for row in rows:
        by_graph.setdefault(row["graph"], []).append(row)
    print("graph constraints bitSupKiB queueP95 queueMax crossRatio hubs bram18 uram288 unknownRate")
    for graph, graph_rows in by_graph.items():
        avg = lambda key: sum(r[key] for r in graph_rows) / len(graph_rows)
        print(
            graph,
            f"{avg('constraints'):.1f}",
            f"{avg('bit_sup_bytes') / 1024.0:.1f}",
            f"{avg('queue_occupancy_p95'):.1f}",
            f"{max(r['queue_occupancy_max'] for r in graph_rows)}",
            f"{avg('cross_event_ratio'):.3f}",
            f"{avg('high_degree_hub_count'):.1f}",
            f"{avg('bram18_estimate'):.1f}",
            f"{avg('uram288_estimate'):.1f}",
            f"{avg('unknown_rate'):.3f}",
        )


if __name__ == "__main__":
    main()
