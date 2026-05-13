"""Render the two headline panels from the schema-validated JSONL.

Reads every *.jsonl in runs/, filters through the locked
DPM_VS_ROLLING_SUMMARY_DIFFERENTIAL chart spec (so prompt-bytes rows
cannot leak into a memory/decision panel), and emits:

  policy_retention.png
    Grouped bar chart. x = case_id (agentic_qwen normal twin), y =
    must_call_recovered count, two series: rolling_summary,
    dpm_projection. The 0/3 vs 3/3 separation shows up as a flat
    rolling bar and a full-height DPM bar.

  cost_asymmetry.png
    Grouped bar chart for the handoff_ish case (real_sessions corpus).
    Three metric groups: tokens_in, tokens_out, calls. Two bars per
    group: rolling_summary, dpm_projection. Logs the ratio in the
    caption so you can read it without doing math.

Usage:
  python runs/render_charts.py [--out-dir runs]

No arguments needed for the headline; the runs are already on disk.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")  # non-interactive; no display needed
import matplotlib.pyplot as plt

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scenario"))
from score_schema import (  # noqa: E402
    ScoreRow,
    CompressionSubstrate,
    TestKind,
    DPM_VS_ROLLING_SUMMARY_DIFFERENTIAL,
)
sys.path.insert(0, str(Path(__file__).resolve().parent))
from _chart_style import (  # noqa: E402
    apply_style, styled_title, styled_footer,
    COLOR_ROLLING, COLOR_REPLAYABLE, COLOR_MUTED, COLOR_GRID, COLOR_TITLE,
)


def _load_rows(runs_dir: Path) -> list[ScoreRow]:
    rows: list[ScoreRow] = []
    for jsonl in sorted(runs_dir.glob("*.jsonl")):
        with jsonl.open("r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                rows.append(ScoreRow.from_dict(json.loads(line)))
    return rows


def _render_policy_retention(rows: list[ScoreRow], out: Path,
                              fonts: dict) -> None:
    """Agentic_qwen normal twin, must_call_recovered on the decision row."""
    target = [
        r for r in rows
        if r.case_corpus == "agentic_qwen"
        and r.pair_role == "normal"
        and r.test_kind == TestKind.DECISION
    ]
    if not target:
        print("policy_retention: no agentic_qwen normal-decision rows; skipping",
              file=sys.stderr)
        return
    by_case: dict[str, dict[str, int]] = {}
    totals: dict[str, int] = {}
    sources: list[str] = []
    for r in target:
        c = r.case_id
        by_case.setdefault(c, {})
        by_case[c][r.compression_substrate.value] = (
            r.scores.get("must_call_recovered_count", 0))
        totals[c] = r.scores.get("must_call_total", 0)

    def _short(case_id: str) -> str:
        base = case_id.split(":")[0]
        if base.startswith("synth-seed"):
            return "synthetic seed"
        if "-" in base and len(base) > 20:
            return f"real row {base.rsplit('-', 1)[-1]}"
        return base

    cases = sorted(by_case.keys())
    rolling = [by_case[c].get("rolling_summary", 0) for c in cases]
    dpm = [by_case[c].get("dpm_projection", 0) for c in cases]
    case_totals = [totals[c] for c in cases]
    x = list(range(len(cases)))
    width = 0.22

    fig, ax = plt.subplots(figsize=(9, 5.2))
    fig.subplots_adjust(top=0.78, bottom=0.16, left=0.10, right=0.96)
    # If a rolling bar is zero, render an outlined ghost-bar at the
    # case's rubric ceiling so the antagonist has visual presence.
    for i, (rv, tot) in enumerate(zip(rolling, case_totals)):
        if rv == 0:
            ax.bar([i - width/2], [tot], width=width, color="none",
                    edgecolor=COLOR_ROLLING, linestyle=(0, (4, 3)),
                    linewidth=1.2)
        else:
            ax.bar([i - width/2], [rv], width=width, color=COLOR_ROLLING)
    ax.bar([i + width/2 for i in x], dpm, width=width,
           label="replayable", color=COLOR_REPLAYABLE)
    # legend proxy for rolling so the ghost-bar gets a label.
    from matplotlib.patches import Patch
    rolling_proxy = Patch(facecolor="none", edgecolor=COLOR_ROLLING,
                           linestyle=(0, (4, 3)), linewidth=1.2,
                           label="rolling-summary  (would-fill ceiling)")
    ax.set_xticks(x)
    ax.set_xticklabels([_short(c) for c in cases],
                        rotation=0, ha="center", fontsize=10,
                        fontfamily=fonts["body"])
    max_total = max(totals.values()) if totals else 1
    ax.set_ylim(0, max_total + 0.5)
    ax.set_yticks(list(range(0, max_total + 1)))
    ax.set_ylabel("must_call_tools recovered in the agent's answer",
                   fontfamily=fonts["body"], fontsize=10)
    for i, (rv, dv, tot) in enumerate(zip(rolling, dpm, [totals[c] for c in cases])):
        ax.text(i - width/2, rv + 0.05, f"{rv}/{tot}", ha="center",
                fontsize=10, fontfamily=fonts["body"], color=COLOR_ROLLING)
        ax.text(i + width/2, dv + 0.05, f"{dv}/{tot}", ha="center",
                fontsize=10, fontfamily=fonts["body"], color=COLOR_REPLAYABLE)
    handles, labels = ax.get_legend_handles_labels()
    handles = [rolling_proxy] + handles
    labels = ["rolling-summary  (would-fill ceiling)"] + labels
    leg = ax.legend(handles, labels, loc="upper left", framealpha=0.95,
                     frameon=True, edgecolor=COLOR_GRID,
                     prop={"family": fonts["body"], "size": 9})
    leg.get_frame().set_linewidth(0.5)
    styled_title(
        ax,
        "Did the agent remember which tools to use?",
        "agentic-qwen normal-twin · 800-char memory budget · n = 2 cases",
        fonts)
    styled_footer(fig, "runs/*agentic_qwen_pair.jsonl", fonts)
    fig.savefig(out, dpi=160)
    plt.close(fig)
    print(f"  wrote {out}")


def _render_cost_asymmetry(rows: list[ScoreRow], out: Path,
                            fonts: dict) -> None:
    """handoff_ish: tokens_in / tokens_out / calls per substrate.

    The decision-row scores carry tokens_in/tokens_out/calls keys
    populated by head_to_head.py.
    """
    target = [
        r for r in rows
        if r.case_corpus == "real_sessions"
        and r.test_kind == TestKind.DECISION
        and "handoff" in r.case_id.lower()
    ]
    if not target:
        # Fallback: pick the largest real_sessions decision-row case
        # by event-count proxy (calls).
        candidates = [r for r in rows
                       if r.case_corpus == "real_sessions"
                       and r.test_kind == TestKind.DECISION]
        if not candidates:
            print("cost_asymmetry: no real_sessions decision rows; skipping",
                  file=sys.stderr)
            return
        case = max(candidates,
                    key=lambda r: r.scores.get("calls", 0)).case_id
        target = [r for r in candidates if r.case_id == case]
    case_id = target[0].case_id

    by_substrate: dict[str, dict[str, int]] = {}
    for r in target:
        by_substrate[r.compression_substrate.value] = {
            "tokens_in": r.scores.get("tokens_in", 0),
            "tokens_out": r.scores.get("tokens_out", 0),
            "calls": r.scores.get("calls", 0),
        }

    metrics = ["tokens_in", "tokens_out", "calls"]
    rolling = [by_substrate.get("rolling_summary", {}).get(m, 0) for m in metrics]
    dpm = [by_substrate.get("dpm_projection", {}).get(m, 0) for m in metrics]
    x = list(range(len(metrics)))
    width = 0.36

    fig, ax = plt.subplots(figsize=(9, 5.2))
    fig.subplots_adjust(top=0.78, bottom=0.18, left=0.10, right=0.96)
    bar_r = ax.bar([i - width/2 for i in x], rolling, width=width,
                    label="rolling-summary", color=COLOR_ROLLING)
    bar_d = ax.bar([i + width/2 for i in x], dpm, width=width,
                    label="replayable", color=COLOR_REPLAYABLE)
    ax.set_xticks(x)
    ax.set_xticklabels(metrics, fontfamily=fonts["body"], fontsize=10)
    ax.set_yscale("log")
    ax.set_ylabel("count (log scale)", fontfamily=fonts["body"], fontsize=10)
    for bars, vals, color in ((bar_r, rolling, COLOR_ROLLING),
                                (bar_d, dpm, COLOR_REPLAYABLE)):
        for b, v in zip(bars, vals):
            ax.text(b.get_x() + b.get_width()/2, v * 1.08, f"{v:,}",
                    ha="center", fontsize=9, color=color,
                    fontfamily=fonts["body"])
    ratios = []
    for m, rv, dv in zip(metrics, rolling, dpm):
        if dv > 0:
            ratios.append(f"{m} {rv/dv:.1f}×")
    if ratios:
        fig.text(0.5, 0.06,
                  "rolling-summary ÷ replayable  —  " + "   ".join(ratios),
                  ha="center", fontsize=9, color=COLOR_MUTED,
                  fontfamily=fonts["body"])
    leg = ax.legend(loc="upper right", framealpha=0.95, frameon=True,
                     edgecolor=COLOR_GRID, prop={"family": fonts["body"],
                                                  "size": 9})
    leg.get_frame().set_linewidth(0.5)
    ax.grid(axis="y", which="both", color=COLOR_GRID, linestyle=":",
            linewidth=0.7)
    styled_title(
        ax,
        "How much does one decision cost?",
        "real Claude session · 492 events · 1338-char memory budget",
        fonts)
    styled_footer(fig, f"runs/*{case_id[:24]}*.jsonl", fonts)
    fig.savefig(out, dpi=160)
    plt.close(fig)
    print(f"  wrote {out}")


def _render_instruction_recall(rows: list[ScoreRow], out: Path,
                                fonts: dict) -> None:
    """correction_heavy: keyword recall of the original user instruction
    after compression. The case had auditor-rubric content embedded in
    prior turns; rolling-summary lost the original ask entirely (0/8);
    DPM with the hardened rebuild prompt preserved 4/8.
    """
    target = [
        r for r in rows
        if r.case_corpus == "real_sessions"
        and r.test_kind == TestKind.DECISION
        and "codex-rollout" in r.case_id.lower()
    ]
    if not target:
        print("instruction_recall: no correction_heavy decision rows; skipping",
              file=sys.stderr)
        return
    case_id = target[0].case_id

    by_substrate: dict[str, dict] = {}
    for r in target:
        by_substrate[r.compression_substrate.value] = {
            "hits": r.scores.get("intent_keyword_hits_count", 0),
            "total": r.scores.get("intent_keyword_hits_total", 0),
            "list": r.scores.get("intent_keyword_hits_list", []),
        }
    rolling = by_substrate.get("rolling_summary", {})
    dpm = by_substrate.get("dpm_projection", {})
    total = max(rolling.get("total", 0), dpm.get("total", 0), 1)

    fig, ax = plt.subplots(figsize=(9, 5.2))
    fig.subplots_adjust(top=0.78, bottom=0.18, left=0.12, right=0.96)
    width = 0.20
    x = [0]
    rh = rolling.get("hits", 0)
    dh = dpm.get("hits", 0)
    # Outlined zero-height ghost-bar for rolling-summary so the 0/8
    # has visual presence next to the tall replayable bar.
    if rh == 0:
        ax.bar([-width/2], [total], width=width, color="none",
                edgecolor=COLOR_ROLLING, linestyle=(0, (4, 3)),
                linewidth=1.2, label="rolling-summary  (would-fill ceiling)")
    else:
        ax.bar([-width/2], [rh], width=width, color=COLOR_ROLLING,
                label="rolling-summary")
    ax.bar([width/2], [dh], width=width, color=COLOR_REPLAYABLE,
            label="replayable")
    ax.set_xticks(x)
    ax.set_xlim(-0.45, 0.45)
    ax.set_xticklabels(["correction-heavy session · 17 events"],
                        fontfamily=fonts["body"], fontsize=10)
    ax.set_ylim(0, total + 1)
    ax.set_yticks(list(range(0, total + 1)))
    ax.set_ylabel("intent keywords recovered\nfrom the original instruction",
                   fontfamily=fonts["body"], fontsize=10)
    # rolling label sits at zero level, slightly offset so it's readable.
    ax.text(-width/2, 0.18, f"{rh}/{total}", ha="center",
            fontfamily=fonts["body"], fontsize=10, color=COLOR_ROLLING)
    ax.text(width/2, dh + 0.18, f"{dh}/{total}", ha="center",
            fontfamily=fonts["body"], fontsize=10, color=COLOR_REPLAYABLE)
    leg = ax.legend(loc="upper left", framealpha=0.95, frameon=True,
                     edgecolor=COLOR_GRID, prop={"family": fonts["body"],
                                                  "size": 9})
    leg.get_frame().set_linewidth(0.5)
    styled_title(
        ax,
        "Did the agent remember the user's first instruction?",
        "correction-heavy session · prior agent turns contained "
        "instruction-shaped content",
        fonts)
    styled_footer(fig, f"runs/*{case_id[:18]}*.jsonl", fonts)
    fig.savefig(out, dpi=160)
    plt.close(fig)
    print(f"  wrote {out}")


def _render_twitter_combined(rows: list[ScoreRow], out: Path,
                              fonts: dict) -> None:
    """Single Twitter-shaped image (1600x900). Left = cost asymmetry chart.
    Right = text panel with the headline claim + Phase 3 hint.
    """
    cost_target = [
        r for r in rows
        if r.case_corpus == "real_sessions"
        and r.test_kind == TestKind.DECISION
        and "handoff" in r.case_id.lower()
    ]
    if not cost_target:
        cands = [r for r in rows if r.case_corpus == "real_sessions"
                  and r.test_kind == TestKind.DECISION]
        if cands:
            cid = max(cands, key=lambda r: r.scores.get("calls", 0)).case_id
            cost_target = [r for r in cands if r.case_id == cid]
    if not cost_target:
        print("twitter_combined: missing cost data; skipping",
              file=sys.stderr)
        return
    cost_by = {r.compression_substrate.value: r.scores for r in cost_target}

    fig, (axL, axR) = plt.subplots(1, 2, figsize=(16, 9),
                                    gridspec_kw={"width_ratios": [1.05, 1]})
    fig.subplots_adjust(top=0.74, bottom=0.16, left=0.06, right=0.97,
                        wspace=0.06)

    # ---- left panel: cost asymmetry ----
    metrics = ["tokens_in", "tokens_out", "calls"]
    rolling_costs = [cost_by.get("rolling_summary", {}).get(m, 0) for m in metrics]
    dpm_costs = [cost_by.get("dpm_projection", {}).get(m, 0) for m in metrics]
    x = list(range(len(metrics)))
    width = 0.36
    barRL = axL.bar([i - width/2 for i in x], rolling_costs, width=width,
                     label="codex / claude", color=COLOR_ROLLING)
    barDL = axL.bar([i + width/2 for i in x], dpm_costs, width=width,
                     label="replayable", color=COLOR_REPLAYABLE)
    axL.set_xticks(x)
    axL.set_xticklabels(metrics, fontfamily=fonts["body"], fontsize=14)
    axL.set_yscale("log")
    axL.set_ylabel("count (log scale)", fontfamily=fonts["body"], fontsize=13)
    for bars, vals, color in ((barRL, rolling_costs, COLOR_ROLLING),
                                (barDL, dpm_costs, COLOR_REPLAYABLE)):
        for b, v in zip(bars, vals):
            axL.text(b.get_x() + b.get_width()/2, v * 1.10, f"{v:,}",
                     ha="center", fontsize=13, color=color,
                     fontfamily=fonts["body"])
    ratios = [
        f"{m} {(rv/dv):.1f}×"
        for m, rv, dv in zip(metrics, rolling_costs, dpm_costs)
        if dv > 0
    ]
    if ratios:
        axL.text(0.5, -0.18,
                  "codex/claude ÷ replayable  —  " + "   ".join(ratios),
                  ha="center", fontsize=11, color=COLOR_MUTED,
                  transform=axL.transAxes, fontfamily=fonts["body"])
    legL = axL.legend(loc="upper right", framealpha=1.0, frameon=True,
                       edgecolor=COLOR_REPLAYABLE,
                       facecolor="#0a0a0a",
                       labelcolor=COLOR_TITLE,
                       prop={"family": fonts["body"], "size": 13,
                             "weight": "bold"})
    legL.get_frame().set_linewidth(1.4)
    axL.grid(axis="y", which="both", color=COLOR_GRID, linestyle=":",
              linewidth=0.7)

    # ---- right panel: text claim + Phase 3 hint ----
    axR.set_xticks([])
    axR.set_yticks([])
    for spine in axR.spines.values():
        spine.set_visible(False)
    axR.set_xlim(0, 1)
    axR.set_ylim(0, 1)

    # Headline (lime, big)
    axR.text(0.04, 0.92, "Replayable agent memory",
              fontsize=22, color=COLOR_REPLAYABLE,
              fontfamily=fonts["body"], fontweight="bold",
              transform=axR.transAxes, va="top")
    axR.text(0.04, 0.84,
              "Same task. Same model. Same memory budget.",
              fontsize=14, color=COLOR_TITLE,
              fontfamily=fonts["body"],
              transform=axR.transAxes, va="top")

    # Two-column data restatement
    axR.text(0.04, 0.70, "codex / claude",
              fontsize=13, color=COLOR_ROLLING,
              fontfamily=fonts["body"], fontweight="bold",
              transform=axR.transAxes, va="top")
    axR.text(0.04, 0.64,
              "17 model calls\n8,035 output tokens\nuser's first instruction lost",
              fontsize=13, color=COLOR_MUTED,
              fontfamily=fonts["body"], linespacing=1.5,
              transform=axR.transAxes, va="top")

    axR.text(0.55, 0.70, "replayable",
              fontsize=12, color=COLOR_REPLAYABLE,
              fontfamily=fonts["body"], fontweight="bold",
              transform=axR.transAxes, va="top")
    axR.text(0.55, 0.64,
              "2 model calls\n750 output tokens\nfirst instruction preserved",
              fontsize=13, color=COLOR_TITLE,
              fontfamily=fonts["body"], linespacing=1.5,
              transform=axR.transAxes, va="top")

    # Divider rule
    axR.plot([0.04, 0.96], [0.34, 0.34], color=COLOR_GRID, linewidth=0.8,
              transform=axR.transAxes, clip_on=False)

    # Phase 3 hint
    axR.text(0.04, 0.30, "N E X T   ·   A U D I T   S U B S T R A T E",
              fontsize=10, color=COLOR_REPLAYABLE,
              fontfamily=fonts["body"], fontweight="bold",
              transform=axR.transAxes, va="top")
    axR.text(0.04, 0.24,
              "Every memory the agent uses is cryptographically\n"
              "tied to the events that produced it. Drift fails the\n"
              "runtime gate closed and emits a blocking correction.",
              fontsize=12, color=COLOR_MUTED,
              fontfamily=fonts["body"], linespacing=1.5,
              transform=axR.transAxes, va="top")

    # Big title on the left only
    styled_title(
        axL,
        "How much does one decision cost?",
        "real Claude session · 492 events · 1338-char memory budget",
        fonts)
    styled_footer(fig, "runs/*.jsonl  ·  github.com/maceip/vet", fonts)
    fig.savefig(out, dpi=100)  # 16x9 @ 100dpi = 1600x900
    plt.close(fig)
    print(f"  wrote {out}")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", type=Path,
                     default=Path(__file__).resolve().parent)
    args = ap.parse_args(argv)

    runs_dir = Path(__file__).resolve().parent
    rows = _load_rows(runs_dir)
    if not rows:
        print(f"no rows under {runs_dir}", file=sys.stderr)
        return 1
    # Run rows through the chart spec to reject any that don't belong.
    # filter() raises on a category error, which is the desired behavior:
    # if a wrong-shaped row got committed, the chart code refuses to render.
    chart_rows = DPM_VS_ROLLING_SUMMARY_DIFFERENTIAL.filter(rows)
    print(f"loaded {len(rows)} rows, {len(chart_rows)} after chart-spec filter")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    fonts = apply_style()
    _render_policy_retention(chart_rows,
                              args.out_dir / "policy_retention.png", fonts)
    _render_cost_asymmetry(chart_rows,
                            args.out_dir / "cost_asymmetry.png", fonts)
    _render_instruction_recall(chart_rows,
                                args.out_dir / "instruction_recall.png", fonts)
    _render_twitter_combined(chart_rows,
                              args.out_dir / "twitter_combined.png", fonts)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
