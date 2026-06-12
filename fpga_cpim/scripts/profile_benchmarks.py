#!/usr/bin/env python3
import argparse
import json
import math
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from hardware_profiles import profile_fit_map


DEFAULT_INPUTS = [
    "tests/data/bench/rand-2-23/rand-23-23-253-131-50021_ext.xml",
    "benchmarks/tightness0.2/rand-2-40-11-414-200-0_ext.xml",
    "benchmarks/tightness0.35/rand-2-40-16-250-350-0_ext.xml",
    "tests/data/bench/queens-12_ext.xml",
    "benchmarks/haystacks/haystacks-11.xml",
]


def repo_root():
    return Path(__file__).resolve().parents[2]


def parse_domain_text(text):
    if not text:
        return 0
    text = text.strip()
    if ".." in text and " " not in text:
        lo, hi = text.split("..", 1)
        return int(hi) - int(lo) + 1
    return len([token for token in text.replace("|", " ").split() if token])


def local_name(tag):
    return tag.split("}", 1)[-1]


def find_children(root, name):
    return [elem for elem in root.iter() if local_name(elem.tag) == name]


def attr_u32(elem, name, default=0):
    try:
        return int(elem.attrib.get(name, default))
    except ValueError:
        return default


def word_count(domain_size):
    return int(math.ceil(domain_size / 32.0)) if domain_size else 0


def estimate_bitsup_bytes(binary_scopes, var_domain_sizes, max_domain):
    total_words = 0
    fallback_words = 2 * max_domain * word_count(max_domain)
    for scope in binary_scopes:
        if len(scope) != 2:
            total_words += fallback_words
            continue
        dx = var_domain_sizes.get(scope[0], max_domain)
        dy = var_domain_sizes.get(scope[1], max_domain)
        total_words += dx * word_count(dy)
        total_words += dy * word_count(dx)
    return total_words * 4


def parse_xcsp_profile(path):
    tree = ET.parse(path)
    root = tree.getroot()

    domain_sizes = {}
    for domain in find_children(root, "domain"):
        name = domain.attrib.get("name")
        size = attr_u32(domain, "nbValues", parse_domain_text(domain.text))
        if name:
            domain_sizes[name] = size

    variables = find_children(root, "variable")
    var_names = []
    var_domain_sizes = {}
    for index, var in enumerate(variables):
        name = var.attrib.get("name", f"V{index}")
        domain_ref = var.attrib.get("domain")
        var_names.append(name)
        var_domain_sizes[name] = domain_sizes.get(domain_ref, 0)
    vars_count = len(var_names)
    max_domain = max(var_domain_sizes.values() or [0])

    constraints = find_children(root, "constraint")
    constraints_count = len(constraints)
    fanout = {name: 0 for name in var_names}
    binary_scopes = []
    for constraint in constraints:
        arity = attr_u32(constraint, "arity", 0)
        scope = constraint.attrib.get("scope", "").split()
        if arity == 2 and len(scope) == 2:
            binary_scopes.append(scope)
            for name in scope:
                if name in fanout:
                    fanout[name] += 1

    binary_count = len(binary_scopes)
    avg_fanout = round(sum(fanout.values()) / vars_count, 3) if vars_count else 0.0
    max_fanout = max(fanout.values() or [0])
    binary_ratio = round(binary_count / constraints_count, 6) if constraints_count else 0.0
    estimated_bitsup = estimate_bitsup_bytes(
        binary_scopes, var_domain_sizes, max_domain
    )

    return {
        "path": str(path),
        "vars": vars_count,
        "max_domain": max_domain,
        "constraints": constraints_count,
        "binary_constraints": binary_count,
        "binary_ratio": binary_ratio,
        "avg_fanout": avg_fanout,
        "max_fanout": max_fanout,
        "estimated_bitSup_bytes": estimated_bitsup,
        "z7020_profile_fit": profile_fit_map(
            vars_count, constraints_count, max_domain
        ),
    }


def resolve_inputs(root, inputs):
    paths = []
    for item in inputs:
        path = Path(item)
        if not path.is_absolute():
            path = root / path
        paths.append(path)
    return paths


def write_jsonl(rows, output_path):
    if output_path == "-":
        for row in rows:
            print(json.dumps(row, ensure_ascii=False, sort_keys=True))
        return
    path = Path(output_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as stream:
        for row in rows:
            stream.write(json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n")


def main():
    parser = argparse.ArgumentParser(
        description="Profile existing XCSP benchmarks for FPGA sizing."
    )
    parser.add_argument("--repo-root", default=str(repo_root()))
    parser.add_argument("--output", default="-")
    parser.add_argument("--inputs", nargs="*", default=DEFAULT_INPUTS)
    args = parser.parse_args()

    root = Path(args.repo_root).resolve()
    rows = []
    had_error = False
    for path in resolve_inputs(root, args.inputs):
        try:
            rows.append(parse_xcsp_profile(path))
        except Exception as exc:  # Keep profiling best-effort for mixed corpora.
            had_error = True
            rows.append({"path": str(path), "error": str(exc)})
    write_jsonl(rows, args.output)
    return 1 if had_error else 0


if __name__ == "__main__":
    sys.exit(main())
