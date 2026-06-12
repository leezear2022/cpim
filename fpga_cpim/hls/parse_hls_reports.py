#!/usr/bin/env python3
import argparse
import json
import re
import shutil
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


DEFAULT_TOP = "cpim_top_hls"


def local_name(tag):
    return tag.split("}", 1)[-1]


def find_numeric_text(root, names):
    normalized = {name.lower().replace("_", "").replace("-", "") for name in names}
    for elem in root.iter():
        name = local_name(elem.tag).lower().replace("_", "").replace("-", "")
        if name not in normalized or elem.text is None:
            continue
        match = re.search(r"-?\d+(?:\.\d+)?", elem.text)
        if match:
            return float(match.group(0))
    return None


def parse_int(value):
    if value is None:
        return 0
    return int(round(float(value)))


def parse_float(value):
    if value is None:
        return 0.0
    return float(value)


def parse_csynth_xml(path):
    root = ET.parse(path).getroot()
    clock_ns = parse_float(
        find_numeric_text(root, ["EstimatedClockPeriod", "EstimatedClock"])
    )
    fmax = round(1000.0 / clock_ns, 3) if clock_ns > 0 else 0.0
    return {
        "latency_min": parse_int(
            find_numeric_text(root, ["Best-caseLatency", "LatencyMin", "MinLatency"])
        ),
        "latency_max": parse_int(
            find_numeric_text(root, ["Worst-caseLatency", "LatencyMax", "MaxLatency"])
        ),
        "ii": parse_int(
            find_numeric_text(root, ["Interval-min", "PipelineII", "InitiationInterval"])
        ),
        "lut": parse_int(find_numeric_text(root, ["LUT"])),
        "ff": parse_int(find_numeric_text(root, ["FF"])),
        "bram": parse_int(find_numeric_text(root, ["BRAM_18K", "BRAM"])),
        "dsp": parse_int(find_numeric_text(root, ["DSP48E", "DSP"])),
        "estimated_fmax_mhz": fmax,
    }


def parse_first_int_after(label, text):
    pattern = rf"{re.escape(label)}[^0-9-]*(-?\d+)"
    match = re.search(pattern, text, flags=re.IGNORECASE)
    return int(match.group(1)) if match else 0


def parse_csynth_rpt(path):
    text = path.read_text(encoding="utf-8", errors="ignore")
    clock_match = re.search(
        r"Estimated\s+Clock\s+Period[^0-9]*(\d+(?:\.\d+)?)",
        text,
        flags=re.IGNORECASE,
    )
    clock_ns = float(clock_match.group(1)) if clock_match else 0.0
    fmax = round(1000.0 / clock_ns, 3) if clock_ns > 0 else 0.0

    resources = {"lut": 0, "ff": 0, "bram": 0, "dsp": 0}
    total_line = re.search(r"\|\s*Total\s*\|([^\n]+)", text)
    if total_line:
        nums = [int(v) for v in re.findall(r"\d+", total_line.group(1))]
        if len(nums) >= 4:
            resources["bram"] = nums[0]
            resources["dsp"] = nums[1]
            resources["ff"] = nums[2]
            resources["lut"] = nums[3]

    return {
        "latency_min": parse_first_int_after("Latency", text),
        "latency_max": parse_first_int_after("Latency", text),
        "ii": parse_first_int_after("Interval", text),
        "estimated_fmax_mhz": fmax,
        **resources,
    }


def status_from_log(path):
    if not path.exists():
        return "not_run"
    text = path.read_text(encoding="utf-8", errors="ignore").lower()
    if "error" in text or "failed" in text:
        return "fail"
    if "pass" in text or "done" in text or "finished" in text:
        return "pass"
    return "pass"


def first_existing(paths):
    for path in paths:
        if path.exists():
            return path
    return None


def build_summary(project, solution, top):
    summary = {
        "tool": "vitis_hls",
        "tool_status": "ok" if shutil.which("vitis_hls") else "tool_missing",
        "solution": solution,
        "csim": "not_run",
        "csynth": "not_run",
        "cosim": "not_run",
        "latency_min": 0,
        "latency_max": 0,
        "ii": 0,
        "lut": 0,
        "ff": 0,
        "bram": 0,
        "dsp": 0,
        "estimated_fmax_mhz": 0.0,
    }

    solution_dir = project / solution
    csim_log = first_existing(
        [
            solution_dir / "csim" / "report" / f"{top}_csim.log",
            solution_dir / "csim" / "build" / "csim.log",
        ]
    )
    if csim_log is not None:
        summary["csim"] = status_from_log(csim_log)

    xml_report = solution_dir / "syn" / "report" / f"{top}_csynth.xml"
    rpt_report = solution_dir / "syn" / "report" / f"{top}_csynth.rpt"
    if xml_report.exists():
        summary.update(parse_csynth_xml(xml_report))
        summary["csynth"] = "pass"
    elif rpt_report.exists():
        summary.update(parse_csynth_rpt(rpt_report))
        summary["csynth"] = "pass"
    elif summary["tool_status"] == "tool_missing":
        summary["csynth"] = "tool_missing"

    cosim_log = first_existing(
        [
            solution_dir / "sim" / "report" / f"{top}_cosim.rpt",
            solution_dir / "sim" / "verilog" / "xsim.log",
        ]
    )
    if cosim_log is not None:
        summary["cosim"] = status_from_log(cosim_log)

    return summary


def main():
    parser = argparse.ArgumentParser(
        description="Extract a stable JSON summary from Vitis HLS reports."
    )
    parser.add_argument("--project", default="fpga_cpim_hls")
    parser.add_argument("--solution", default="z7020_small")
    parser.add_argument("--top", default=DEFAULT_TOP)
    parser.add_argument("--output", default="-")
    args = parser.parse_args()

    summary = build_summary(Path(args.project), args.solution, args.top)
    text = json.dumps(summary, sort_keys=True) + "\n"
    if args.output == "-":
        sys.stdout.write(text)
    else:
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
