#!/usr/bin/env python3
import argparse
import json
import subprocess
from pathlib import Path


def run_case(binary, graph, domain, support_banks, seed, args):
    cmd = [
        str(binary),
        "--instance",
        f"synthetic:{graph}",
        "--vars",
        str(args.vars),
        "--domain",
        str(domain),
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
        "--support-banks",
        str(support_banks),
        "--support-base-latency",
        str(args.support_base_latency),
        "--support-conflict-penalty",
        str(args.support_conflict_penalty),
    ]
    proc = subprocess.run(cmd, text=True, capture_output=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip())
    data = json.loads(proc.stdout)
    return {
        "graph": graph,
        "domain": domain,
        "support_banks": support_banks,
        "seed": seed,
        "constraints": data["model"]["num_constraints"],
        "bit_sup_bytes": data["model"]["bit_sup_bytes"],
        "fanout_p95": data["telemetry"]["fanout_p95"],
        "queue_occupancy_p95": data["telemetry"]["queue_occupancy_p95"],
        "queue_occupancy_max": data["telemetry"]["queue_occupancy_max"],
        "support_latency_cycles": data["telemetry"]["support_latency_cycles"],
        "support_bank_conflicts": data["telemetry"]["support_bank_conflicts"],
        "support_max_bank_accesses": data["telemetry"]["support_max_bank_accesses"],
        "support_words_touched": data["telemetry"]["support_words_touched"],
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
    parser.add_argument("--domains", default="")
    parser.add_argument("--density", type=float, default=0.2)
    parser.add_argument("--tightness", type=float, default=0.5)
    parser.add_argument("--mode", default="nsacq")
    parser.add_argument("--worlds", type=int, default=4)
    parser.add_argument("--partitions", type=int, default=4)
    parser.add_argument("--support-banks", type=int, default=4)
    parser.add_argument("--support-banks-list", default="")
    parser.add_argument("--support-base-latency", type=int, default=1)
    parser.add_argument("--support-conflict-penalty", type=int, default=1)
    parser.add_argument("--jsonl", default="")
    args = parser.parse_args()

    binary = Path(args.binary)
    domains = (
        [int(d.strip()) for d in args.domains.split(",") if d.strip()]
        if args.domains
        else [args.domain]
    )
    support_banks_values = (
        [int(b.strip()) for b in args.support_banks_list.split(",") if b.strip()]
        if args.support_banks_list
        else [args.support_banks]
    )
    rows = []
    for graph in [g.strip() for g in args.graphs.split(",") if g.strip()]:
        for domain in domains:
            for support_banks in support_banks_values:
                for seed in range(args.seeds):
                    rows.append(run_case(binary, graph, domain, support_banks, seed, args))

    if args.jsonl:
        with open(args.jsonl, "w", encoding="utf-8") as f:
            for row in rows:
                f.write(json.dumps(row, ensure_ascii=False) + "\n")

    by_graph = {}
    for row in rows:
        by_graph.setdefault(row["graph"], []).append(row)
    print(
        "graph domain banks constraints bitSupKiB queueP95 queueMax "
        "latencyM bankConflicts conflictRate maxBankAccess crossRatio hubs bram18 uram288 unknownRate"
    )
    for graph, graph_rows in by_graph.items():
        by_domain = {}
        for row in graph_rows:
            by_domain.setdefault(row["domain"], []).append(row)
        for domain, domain_rows in sorted(by_domain.items()):
            by_banks = {}
            for row in domain_rows:
                by_banks.setdefault(row["support_banks"], []).append(row)
            for banks, bank_rows in sorted(by_banks.items()):
                avg = lambda key: sum(r[key] for r in bank_rows) / len(bank_rows)
                conflict_rate = (
                    avg("support_bank_conflicts") / avg("support_words_touched")
                    if avg("support_words_touched") > 0
                    else 0.0
                )
                print(
                    graph,
                    domain,
                    banks,
                    f"{avg('constraints'):.1f}",
                    f"{avg('bit_sup_bytes') / 1024.0:.1f}",
                    f"{avg('queue_occupancy_p95'):.1f}",
                    f"{max(r['queue_occupancy_max'] for r in bank_rows)}",
                    f"{avg('support_latency_cycles') / 1_000_000.0:.2f}",
                    f"{avg('support_bank_conflicts'):.1f}",
                    f"{conflict_rate:.4f}",
                    f"{max(r['support_max_bank_accesses'] for r in bank_rows)}",
                    f"{avg('cross_event_ratio'):.3f}",
                    f"{avg('high_degree_hub_count'):.1f}",
                    f"{avg('bram18_estimate'):.1f}",
                    f"{avg('uram288_estimate'):.1f}",
                    f"{avg('unknown_rate'):.3f}",
                )


if __name__ == "__main__":
    main()
