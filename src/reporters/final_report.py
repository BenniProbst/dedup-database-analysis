#!/usr/bin/env python3
"""
Generate final experiment report combining all stages.
"""

import argparse
import json
from datetime import datetime
from pathlib import Path


def generate_report(results_dir: Path, output_dir: Path, title: str):
    output_dir.mkdir(parents=True, exist_ok=True)

    stages = {}
    for stage_dir in ["stage1", "stage2", "stage3"]:
        stage_path = results_dir / stage_dir
        if stage_path.exists():
            summary_file = results_dir / f"{stage_dir}_summary.json"
            if summary_file.exists():
                with open(summary_file) as fh:
                    stages[stage_dir] = json.load(fh)

    # Generate text summary
    lines = [
        f"{'=' * 70}",
        f"  {title}",
        f"  Generated: {datetime.now().isoformat()}",
        f"{'=' * 70}",
        "",
    ]

    for stage_name, stage_data in stages.items():
        lines.append(f"\n{'=' * 40}")
        lines.append(f"  {stage_name.upper()}: {stage_data.get('stage', 'N/A')}")
        lines.append(f"{'=' * 40}")

        for sys_name, sys_data in sorted(stage_data.get("systems", {}).items()):
            lines.append(f"\n  {sys_name}:")
            for grade, grade_data in sorted(sys_data.items()):
                delta = grade_data.get("size_delta_bytes", 0)
                edr = grade_data.get("edr", "N/A")
                duration = grade_data.get("duration_ms", 0)
                lines.append(
                    f"    {grade}: delta={delta/(1024*1024):.1f}MiB, "
                    f"EDR={edr}, duration={duration/1000:.1f}s"
                )

    # Key findings
    lines.extend([
        "",
        f"{'=' * 40}",
        "  KEY FINDINGS",
        f"{'=' * 40}",
        "",
        "  Systems with EDR > 1.0 at U90 demonstrate active deduplication.",
        "  Systems with EDR ~ 1.0 at all grades show no deduplication.",
        "  Reclamation behavior indicates whether dedup is reversible.",
        "",
    ])

    summary_text = "\n".join(lines)
    (output_dir / "summary.txt").write_text(summary_text)

    # Generate metrics.env for GitLab CI
    env_lines = []
    for stage_name, stage_data in stages.items():
        for sys_name, sys_data in stage_data.get("systems", {}).items():
            for grade, grade_data in sys_data.items():
                prefix = f"{stage_name}_{sys_name}_{grade}".upper().replace("-", "_")
                if "size_delta_bytes" in grade_data:
                    env_lines.append(f"{prefix}_DELTA={grade_data['size_delta_bytes']}")
                if "edr" in grade_data:
                    env_lines.append(f"{prefix}_EDR={grade_data['edr']}")

    (output_dir / "metrics.env").write_text("\n".join(env_lines))

    print(summary_text)
    print(f"\nReport saved to {output_dir}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--results-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--title", default="Dedup Experiment Report")
    args = parser.parse_args()
    generate_report(args.results_dir, args.output_dir, args.title)


if __name__ == "__main__":
    main()
