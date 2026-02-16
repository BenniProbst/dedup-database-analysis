#!/usr/bin/env python3
"""
Generate charts from experiment results.
Produces bar charts comparing physical storage across systems and duplication grades.
"""

import argparse
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def generate_charts(input_file: Path, output_dir: Path, title: str):
    output_dir.mkdir(parents=True, exist_ok=True)

    with open(input_file) as fh:
        data = json.load(fh)

    systems = sorted(data.get("systems", {}).keys())
    grades = ["U0", "U50", "U90"]

    # Bar chart: Physical storage delta per system and grade
    fig, ax = plt.subplots(figsize=(14, 8))
    x = np.arange(len(systems))
    width = 0.25

    for i, grade in enumerate(grades):
        values = []
        for sys_name in systems:
            sys_data = data["systems"].get(sys_name, {})
            grade_data = sys_data.get(grade, {})
            delta_mb = grade_data.get("size_delta_bytes", 0) / (1024 * 1024)
            values.append(delta_mb)
        ax.bar(x + i * width, values, width, label=grade)

    ax.set_xlabel("System")
    ax.set_ylabel("Physical Storage Delta (MiB)")
    ax.set_title(title)
    ax.set_xticks(x + width)
    ax.set_xticklabels(systems, rotation=45, ha="right")
    ax.legend()
    ax.grid(axis="y", alpha=0.3)

    plt.tight_layout()
    plt.savefig(output_dir / "storage_comparison.png", dpi=150)
    plt.close()

    # EDR chart (if available)
    has_edr = any(
        data["systems"].get(s, {}).get(g, {}).get("edr")
        for s in systems for g in grades
    )
    if has_edr:
        fig, ax = plt.subplots(figsize=(14, 8))
        for i, grade in enumerate(grades):
            values = []
            for sys_name in systems:
                edr = data["systems"].get(sys_name, {}).get(grade, {}).get("edr", "1.0")
                try:
                    values.append(float(edr))
                except (ValueError, TypeError):
                    values.append(1.0)
            ax.bar(x + i * width, values, width, label=grade)

        ax.set_xlabel("System")
        ax.set_ylabel("EDR (Effective Deduplication Ratio)")
        ax.set_title(f"{title} - EDR")
        ax.set_xticks(x + width)
        ax.set_xticklabels(systems, rotation=45, ha="right")
        ax.axhline(y=1.0, color="red", linestyle="--", alpha=0.5, label="No dedup (EDR=1)")
        ax.legend()
        ax.grid(axis="y", alpha=0.3)

        plt.tight_layout()
        plt.savefig(output_dir / "edr_comparison.png", dpi=150)
        plt.close()

    print(f"Charts saved to {output_dir}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--title", default="Dedup Experiment Results")
    args = parser.parse_args()
    generate_charts(args.input, args.output_dir, args.title)


if __name__ == "__main__":
    main()
