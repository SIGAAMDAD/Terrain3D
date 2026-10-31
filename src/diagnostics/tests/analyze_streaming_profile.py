#!/usr/bin/env python3
"""Summarize opt-in Terrain3D P12 profiler snapshots exported as JSON.

Accepted inputs: get_streaming_profiler_stats() JSON, or a wrapper with
`streaming_profile` (or `profiler`) and optional `workload`, `duration_seconds`
and `gpu_ms` fields. Measurements are per-stage, not additive frame timings.
"""

import argparse
import csv
import json
from pathlib import Path
import sys


def load_capture(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    label = data.get("workload", Path(path).stem)
    profile = data.get("streaming_profile", data.get("profiler", data))
    stages = profile.get("stages", {})
    if not isinstance(stages, dict) or not stages:
        raise ValueError(f"{path}: no profiler stage data")
    duration = data.get("duration_seconds", 0)
    if duration and (not isinstance(duration, (float, int)) or duration <= 0):
        raise ValueError(f"{path}: invalid duration_seconds")
    return label, duration, data.get("gpu_ms"), stages


def extract_rows(paths):
    rows = []
    for path in paths:
        workload, duration, gpu_ms, stages = load_capture(path)
        for name, item in stages.items():
            if not name.startswith("h") or len(name) < 3 or name[1:3] not in {
                f"{i:02d}" for i in range(1, 13)
            }:
                continue
            calls = int(item.get("calls", 0))
            samples = int(item.get("timed_samples", 0))
            avg = float(item.get("avg_usec_sampled", 0))
            row = {
                "workload": workload,
                "candidate": name[0:3].upper(),
                "stage": name,
                "calls": calls,
                "timed_samples": samples,
                "calls_per_second": round(calls / duration, 3) if duration else "",
                "avg_usec_sampled": round(avg, 3),
                "p95_usec_upper": item.get("p95_usec_upper", ""),
                "work_unit_name": item.get("work_unit_name", "none"),
                "work_units": item.get("work_units", 0),
                "events_name": item.get("event_name", "none"),
                "events": item.get("events", 0),
                "bytes": item.get("bytes", 0),
                "estimated_cpu_ms_per_second": round(avg * calls / duration / 1000, 3)
                if duration and samples else "",
                "external_gpu_ms": gpu_ms if gpu_ms is not None else "",
            }
            rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", help="JSON files from Terrain3D profiler")
    parser.add_argument("--csv", dest="csv_path", help="Save normalized row table to CSV")
    args = parser.parse_args()
    try:
        rows = extract_rows(args.captures)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        parser.error(str(exc))
    if not rows:
        parser.error("no H01-H12 stages present in supplied capture")
    columns = list(rows[0].keys())
    if args.csv_path:
        with open(args.csv_path, "w", encoding="utf-8", newline="") as fp:
            writer = csv.DictWriter(fp, fieldnames=columns)
            writer.writeheader()
            writer.writerows(rows)
    print("Workload | Candidate | Stage | Calls | Samples | Avg CPU us | P95 upper us | Work units")
    print("--- | --- | --- | ---: | ---: | ---: | ---: | ---:")
    for row in rows:
        print(f"{row['workload']} | {row['candidate']} | {row['stage']} | {row['calls']} | "
              f"{row['timed_samples']} | {row['avg_usec_sampled']} | "
              f"{row['p95_usec_upper']} | {row['work_units']}")
    print("\nTimes are sampled CPU scopes; external_gpu_ms is supplied independently. "
          "Nested stages overlap and must not be summed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
