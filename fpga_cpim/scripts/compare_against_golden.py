#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("json_file")
    parser.add_argument("--allow-unknown", action="store_true")
    args = parser.parse_args()
    data = json.loads(Path(args.json_file).read_text())
    unknown = data["results"]["unknown"]
    if unknown and not args.allow_unknown:
        raise SystemExit(f"unexpected UNKNOWN count: {unknown}")
    print(
        "ok:",
        f"probes={data['results']['probes_total']}",
        f"dwo={data['results']['dwo']}",
        f"unknown={unknown}",
    )


if __name__ == "__main__":
    main()
