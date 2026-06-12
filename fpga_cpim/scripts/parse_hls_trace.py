#!/usr/bin/env python3
import argparse
import csv
import json
import math
import sys


FIELDS = [
    "kind",
    "profile",
    "graph",
    "vars",
    "domain",
    "density",
    "partitions",
    "tiles",
    "capacity",
    "constraints",
    "status",
    "events",
    "epochs",
    "tile_steps",
    "queue_peak_total",
    "queue_peak_partition",
    "local_events",
    "cross_events",
    "deleted_values",
    "router_overflow",
    "semantic_min_capacity",
    "recommended_depth_1p25",
    "recommended_depth_pow2",
    "chosen_depth",
    "chosen_depth_overhead",
]

INT_FIELDS = {
    "vars",
    "domain",
    "partitions",
    "tiles",
    "capacity",
    "constraints",
    "events",
    "epochs",
    "tile_steps",
    "queue_peak_total",
    "queue_peak_partition",
    "local_events",
    "cross_events",
    "deleted_values",
    "router_overflow",
    "semantic_min_capacity",
    "recommended_depth_1p25",
    "recommended_depth_pow2",
    "chosen_depth",
}


def convert_value(key, value):
    if key in INT_FIELDS:
        return int(value)
    if key == "density":
        return float(value)
    return value


def parse_line(line):
    parts = line.strip().split()
    if not parts or parts[0] not in {"hls_pressure", "hls_capacity"}:
        return None
    row = {"kind": parts[0][4:]}
    for token in parts[1:]:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        row[key] = convert_value(key, value)
    return row


def read_rows(stream, kind):
    rows = []
    for line in stream:
        row = parse_line(line)
        if row is None:
            continue
        if kind != "all" and row["kind"] != kind:
            continue
        rows.append(row)
    return rows


def next_power_of_two(value):
    if value <= 1:
        return 1
    return 1 << (value - 1).bit_length()


def fixture_key(row):
    return (
        row.get("profile", ""),
        row.get("graph", ""),
        row.get("vars", 0),
        row.get("domain", 0),
        row.get("density", 0.0),
        row.get("partitions", 0),
        row.get("constraints", 0),
    )


def enrich_sizing(rows):
    groups = {}
    for row in rows:
        groups.setdefault(fixture_key(row), []).append(row)

    for group_rows in groups.values():
        ok_peaks = [
            row.get("queue_peak_partition", 0)
            for row in group_rows
            if row.get("status") == "OK"
        ]
        all_peaks = [row.get("queue_peak_partition", 0) for row in group_rows]
        semantic_min = max(ok_peaks or all_peaks or [0])
        recommended_1p25 = int(math.ceil(semantic_min * 1.25))
        recommended_pow2 = next_power_of_two(semantic_min)
        chosen_depth = max(recommended_1p25, recommended_pow2)
        overhead = round(chosen_depth / semantic_min, 3) if semantic_min else 0.0
        for row in group_rows:
            row["semantic_min_capacity"] = semantic_min
            row["recommended_depth_1p25"] = recommended_1p25
            row["recommended_depth_pow2"] = recommended_pow2
            row["chosen_depth"] = chosen_depth
            row["chosen_depth_overhead"] = overhead
    return rows


def write_jsonl(rows, stream):
    for row in rows:
        stream.write(json.dumps(row, sort_keys=True) + "\n")


def write_csv(rows, stream):
    writer = csv.DictWriter(stream, fieldnames=FIELDS, extrasaction="ignore")
    writer.writeheader()
    for row in rows:
        writer.writerow(row)


def main():
    parser = argparse.ArgumentParser(
        description="Convert hls_pressure/hls_capacity rows to JSONL or CSV."
    )
    parser.add_argument("--input", default="-", help="input text path or '-'")
    parser.add_argument("--output", default="-", help="output path or '-'")
    parser.add_argument("--format", choices=["jsonl", "csv"], default="jsonl")
    parser.add_argument(
        "--kind", choices=["all", "pressure", "capacity"], default="all"
    )
    args = parser.parse_args()

    if args.input == "-":
        rows = read_rows(sys.stdin, args.kind)
    else:
        with open(args.input, "r", encoding="utf-8") as stream:
            rows = read_rows(stream, args.kind)
    rows = enrich_sizing(rows)

    if args.output == "-":
        output = sys.stdout
        close_output = False
    else:
        output = open(args.output, "w", encoding="utf-8", newline="")
        close_output = True

    try:
        if args.format == "jsonl":
            write_jsonl(rows, output)
        else:
            write_csv(rows, output)
    finally:
        if close_output:
            output.close()


if __name__ == "__main__":
    main()
