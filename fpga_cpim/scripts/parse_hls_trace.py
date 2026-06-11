#!/usr/bin/env python3
import argparse
import csv
import json
import sys


FIELDS = [
    "kind",
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
