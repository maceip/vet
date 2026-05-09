"""Small SVG chart helpers for the Phase 3 bench report.

No third-party plotting dependency on purpose. These charts are meant to be
stable in CI and readable in a README or paper draft, not interactive.
"""
from __future__ import annotations

from dataclasses import dataclass
from html import escape
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class Bar:
    label: str
    value: float
    color: str
    detail: str = ""


def write_bar_chart(
    path: str | Path,
    title: str,
    bars: Iterable[Bar],
    *,
    value_label: str,
    max_value: float | None = None,
    width: int = 920,
    row_height: int = 44,
) -> None:
    """Write a horizontal bar chart as SVG."""

    bars = list(bars)
    max_observed = max([b.value for b in bars] + [0.0])
    scale_max = max_value if max_value is not None else max(1.0, max_observed)
    scale_max = max(scale_max, 1e-9)

    margin_left = 250
    margin_right = 150
    margin_top = 72
    margin_bottom = 44
    chart_width = width - margin_left - margin_right
    height = margin_top + margin_bottom + max(1, len(bars)) * row_height

    lines: list[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        f'<text x="24" y="34" font-family="Arial, sans-serif" font-size="22" font-weight="700" fill="#111827">{escape(title)}</text>',
        f'<text x="24" y="57" font-family="Arial, sans-serif" font-size="13" fill="#4b5563">{escape(value_label)}</text>',
        f'<line x1="{margin_left}" y1="{margin_top - 12}" x2="{margin_left + chart_width}" y2="{margin_top - 12}" stroke="#e5e7eb"/>',
    ]

    for i, bar in enumerate(bars):
        y = margin_top + i * row_height
        bar_width = int(round((bar.value / scale_max) * chart_width))
        label = escape(bar.label)
        detail = escape(bar.detail)
        value = _format_value(bar.value)
        lines.extend([
            f'<text x="24" y="{y + 24}" font-family="Arial, sans-serif" font-size="14" fill="#111827">{label}</text>',
            f'<rect x="{margin_left}" y="{y + 5}" width="{chart_width}" height="24" rx="3" fill="#f3f4f6"/>',
            f'<rect x="{margin_left}" y="{y + 5}" width="{bar_width}" height="24" rx="3" fill="{escape(bar.color)}"/>',
            f'<text x="{margin_left + chart_width + 14}" y="{y + 23}" font-family="Arial, sans-serif" font-size="13" fill="#111827">{value}</text>',
        ])
        if detail:
            lines.append(
                f'<text x="{margin_left}" y="{y + 42}" font-family="Arial, sans-serif" font-size="11" fill="#6b7280">{detail}</text>'
            )

    lines.append("</svg>")
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def condition_color(condition: str) -> str:
    if condition == "raw_oracle":
        return "#2563eb"
    if condition == "rolling_summary":
        return "#d97706"
    if condition == "dpm_phase3_checkpoint":
        return "#059669"
    return "#6b7280"


def _format_value(value: float) -> str:
    if abs(value) >= 100:
        return f"{value:.0f}"
    if abs(value) >= 10:
        return f"{value:.1f}"
    return f"{value:.3f}".rstrip("0").rstrip(".")

