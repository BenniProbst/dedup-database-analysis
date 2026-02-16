#!/usr/bin/env python3
"""
Aggregate experiment results from individual JSON files into a summary.
"""

import argparse
import json
from pathlib import Path


def aggregate(input_dir: Path, output: Path, stage: str):
    results = []
    for f in sorted(input_dir.glob("*.json")):
        with open(f) as fh:
            data = json.load(fh)
            if data.get("stage") == stage or stage == "all":
                results.append(data)

    summary = {
        "stage": stage,
        "count": len(results),
        "systems": {},
    }

    for r in results:
        sys_name = r["system"]
        if sys_name not in summary["systems"]:
            summary["systems"][sys_name] = {}
        grade = r.get("dup_grade", "unknown")
        summary["systems"][sys_name][grade] = r

    output.parent.mkdir(parents=True, exist_ok=True)
    with open(output, "w") as fh:
        json.dump(summary, fh, indent=2)

    print(f"Aggregated {len(results)} results into {output}")

    # Print summary table
    print(f"\n{'System':<20} {'Grade':<6} {'Size Delta (MB)':<18} {'Duration (s)':<14}")
    print("-" * 60)
    for r in sorted(results, key=lambda x: (x["system"], x.get("dup_grade", ""))):
        delta_mb = r.get("size_delta_bytes", 0) / (1024 * 1024)
        duration_s = r.get("duration_ms", 0) / 1000
        print(f"{r['system']:<20} {r.get('dup_grade','?'):<6} {delta_mb:<18.2f} {duration_s:<14.2f}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--stage", default="all")
    args = parser.parse_args()
    aggregate(args.input_dir, args.output, args.stage)


if __name__ == "__main__":
    main()
