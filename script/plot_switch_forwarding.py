#!/usr/bin/env python3
"""Count switch forwarding log lines and draw a mode comparison."""

from __future__ import annotations

import argparse
import html
import re
from pathlib import Path


FORWARD_RE = re.compile(r"^(Leaf|Spine)\s+(\d+):\s+forwarded\b")


def count_forwards(log_path: Path) -> dict[str, object]:
    counts = {
        "Leaf": 0,
        "Spine": 0,
        "Total": 0,
        "by_switch": {},
    }

    with log_path.open("r", encoding="utf-8", errors="replace") as file:
        for line in file:
            match = FORWARD_RE.match(line)
            if match is None:
                continue

            role, switch_id = match.groups()
            switch_name = f"{role}{switch_id}"
            counts[role] += 1
            counts["Total"] += 1
            counts["by_switch"][switch_name] = (
                counts["by_switch"].get(switch_name, 0) + 1
            )

    return counts


def svg_text(x: float, y: float, text: str, **attrs: object) -> str:
    normalized = []
    for key, value in attrs.items():
        if key == "class_":
            key = "class"
        else:
            key = key.replace("_", "-")
        normalized.append(f'{key}="{value}"')
    attr_text = " ".join(normalized)
    return f'<text x="{x}" y="{y}" {attr_text}>{html.escape(text)}</text>'


def infer_label(log_path: Path) -> str:
    last_token = log_path.stem.rsplit("_", 1)[-1].lower()
    labels = {
        "b": "Baseline",
        "baseline": "Baseline",
        "i": "ISC",
        "isc": "ISC",
        "r": "RTB",
        "rtb": "RTB",
        "t": "TRACI",
        "traci": "TRACI",
    }
    return labels.get(last_token, log_path.stem)


def write_svg(
    output_path: Path,
    log_paths: list[Path],
    results: list[tuple[str, dict[str, object]]],
) -> None:
    categories = ["Leaf", "Spine", "Total"]
    all_values = [
        int(counts[category])
        for _label, counts in results
        for category in categories
    ]
    max_value = max(all_values + [1])

    width = 920
    height = 560
    margin_left = 90
    margin_right = 40
    margin_top = 82
    margin_bottom = 98
    chart_width = width - margin_left - margin_right
    chart_height = height - margin_top - margin_bottom
    colors = [
        "#4c78a8",
        "#f58518",
        "#54a24b",
        "#e45756",
        "#72b7b2",
        "#b279a2",
    ]

    def y_of(value: int) -> float:
        return margin_top + chart_height - (value / max_value) * chart_height

    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" '
        f'width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        "<style>",
        "text { font-family: Arial, sans-serif; fill: #1f2933; }",
        ".title { font-size: 24px; font-weight: 700; }",
        ".subtitle { font-size: 13px; fill: #52606d; }",
        ".axis { stroke: #9aa5b1; stroke-width: 1; }",
        ".grid { stroke: #e4e7eb; stroke-width: 1; }",
        ".label { font-size: 14px; }",
        ".value { font-size: 13px; font-weight: 700; }",
        "</style>",
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        svg_text(30, 38, "Switch Forwarded Packet Count", class_="title"),
        svg_text(
            30,
            62,
            "    ".join(path.name for path in log_paths),
            class_="subtitle",
        ),
    ]

    tick_count = 5
    for i in range(tick_count + 1):
        value = round(max_value * i / tick_count)
        y = y_of(value)
        parts.append(
            f'<line x1="{margin_left}" y1="{y:.1f}" '
            f'x2="{width - margin_right}" y2="{y:.1f}" class="grid"/>'
        )
        parts.append(svg_text(22, y + 4, str(value), class_="label"))

    parts.append(
        f'<line x1="{margin_left}" y1="{margin_top}" '
        f'x2="{margin_left}" y2="{margin_top + chart_height}" class="axis"/>'
    )
    parts.append(
        f'<line x1="{margin_left}" y1="{margin_top + chart_height}" '
        f'x2="{width - margin_right}" y2="{margin_top + chart_height}" '
        'class="axis"/>'
    )

    group_width = chart_width / len(categories)
    bar_count = len(results)
    gap = 8
    bar_width = min(46, (group_width - 42) / bar_count - gap)
    bar_width = max(18, bar_width)
    bars_total_width = bar_count * bar_width + (bar_count - 1) * gap
    for idx, category in enumerate(categories):
        center = margin_left + group_width * idx + group_width / 2
        start_x = center - bars_total_width / 2

        for result_idx, (label, counts) in enumerate(results):
            value = int(counts[category])
            color = colors[result_idx % len(colors)]
            x = start_x + result_idx * (bar_width + gap)
            y = y_of(value)
            bar_height = margin_top + chart_height - y
            parts.append(
                f'<rect x="{x:.1f}" y="{y:.1f}" width="{bar_width}" '
                f'height="{bar_height:.1f}" fill="{color}" rx="3"/>'
            )
            parts.append(
                svg_text(
                    x + bar_width / 2,
                    y - 8,
                    str(value),
                    class_="value",
                    text_anchor="middle",
                )
            )

        parts.append(
            svg_text(
                center,
                margin_top + chart_height + 22,
                category,
                class_="label",
                text_anchor="middle",
            )
        )

    baseline_total = int(results[0][1]["Total"])
    best_label, best_counts = min(results, key=lambda item: int(item[1]["Total"]))
    best_total = int(best_counts["Total"])
    if baseline_total > 0:
        reduction = (baseline_total - best_total) / baseline_total * 100
        summary = (
            f"Best total: {results[0][0]} {baseline_total} -> "
            f"{best_label} {best_total} "
            f"({reduction:.1f}% fewer forwarded packets)"
        )
    else:
        summary = "Total reduction: first log has no forwarded packets"
    parts.append(svg_text(30, height - 24, summary, class_="subtitle"))

    legend_y = 34
    legend_x = width - 330
    for idx, (label, _counts) in enumerate(results):
        x = legend_x + (idx % 3) * 108
        y = legend_y + (idx // 3) * 24
        color = colors[idx % len(colors)]
        parts.append(
            f'<rect x="{x}" y="{y - 12}" width="14" '
            f'height="14" fill="{color}"/>'
        )
        parts.append(svg_text(x + 20, y, label, class_="label"))

    parts.append("</svg>")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(parts) + "\n", encoding="utf-8")


def print_summary(name: str, counts: dict[str, object]) -> None:
    print(f"{name}:")
    print(f"  Leaf:  {counts['Leaf']}")
    print(f"  Spine: {counts['Spine']}")
    print(f"  Total: {counts['Total']}")
    by_switch = counts["by_switch"]
    if by_switch:
        detail = ", ".join(
            f"{switch}={by_switch[switch]}" for switch in sorted(by_switch)
        )
        print(f"  By switch: {detail}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Count switch forwarded packets in mode logs "
        "and generate an SVG comparison chart."
    )
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("assets/switch_forwarding_compare.svg"),
        help="output SVG path, default: assets/switch_forwarding_compare.svg",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if len(args.logs) < 2:
        raise SystemExit("Error: provide at least two log files")

    for log_path in args.logs:
        if not log_path.is_file():
            raise SystemExit(f"Error: log not found: {log_path}")

    results = []
    for log_path in args.logs:
        label = infer_label(log_path)
        counts = count_forwards(log_path)
        results.append((label, counts))
        print_summary(label, counts)

    write_svg(args.output, args.logs, results)
    print(f"Chart saved to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
