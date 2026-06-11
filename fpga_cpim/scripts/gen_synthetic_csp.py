#!/usr/bin/env python3
import argparse
import json


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--vars", type=int, default=32)
    parser.add_argument("--domain", type=int, default=32)
    parser.add_argument("--density", type=float, default=0.2)
    parser.add_argument("--tightness", type=float, default=0.5)
    parser.add_argument("--graph", default="random")
    parser.add_argument("--degree", type=int, default=0)
    args = parser.parse_args()

    if args.graph == "chain":
        constraints = max(0, args.vars - 1)
    elif args.graph == "hub":
        constraints = max(0, args.vars - 1)
    elif args.degree:
        constraints = args.vars * args.degree // 2
    else:
        constraints = int(args.vars * (args.vars - 1) / 2 * args.density)

    words = (args.domain + 31) // 32
    bit_sup_bytes = constraints * args.domain * words * 2 * 4
    summary = {
        "vars": args.vars,
        "domain": args.domain,
        "graph": args.graph,
        "constraints_estimate": constraints,
        "fanout_average_estimate": (2 * constraints / args.vars) if args.vars else 0,
        "bit_sup_bytes_estimate": bit_sup_bytes,
        "support_density_estimate": max(0.0, min(1.0, 1.0 - args.tightness)),
        "bram18_estimate": (bit_sup_bytes + 2303) // 2304,
        "uram288_estimate": (bit_sup_bytes + 36863) // 36864,
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
