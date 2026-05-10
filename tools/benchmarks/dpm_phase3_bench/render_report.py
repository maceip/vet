"""Render the Phase 3 bench report from schema-validated JSONL rows."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from statistics import mean, stdev
from typing import Iterable

try:
    from tools.benchmarks.dpm_phase3_bench.bench_schema import (
        PHASE3_HANDOFF_QUALITY_CHART,
        AuditVerdict,
        BenchRow,
        BenchRowError,
        Condition,
        ScoreStatus,
        TestKind,
    )
    from tools.benchmarks.dpm_phase3_bench.charts import (
        Bar,
        condition_color,
        write_bar_chart,
    )
except ModuleNotFoundError:  # Allows running from this directory directly.
    from bench_schema import (  # type: ignore
        PHASE3_HANDOFF_QUALITY_CHART,
        AuditVerdict,
        BenchRow,
        BenchRowError,
        Condition,
        ScoreStatus,
        TestKind,
    )
    from charts import Bar, condition_color, write_bar_chart  # type: ignore


QUALITY_KINDS = {
    TestKind.DECISION,
    TestKind.HANDOFF,
    TestKind.CORRECTION_SAFETY,
}


def load_rows(path: str | Path) -> list[BenchRow]:
    rows: list[BenchRow] = []
    for line_no, line in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        try:
            rows.append(BenchRow.from_dict(json.loads(line)))
        except (json.JSONDecodeError, BenchRowError, ValueError) as e:
            raise SystemExit(f"{path}:{line_no}: invalid Phase 3 bench row: {e}") from e
    return rows


def write_report(rows: list[BenchRow], out_dir: str | Path) -> None:
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    (out / "examples").mkdir(exist_ok=True)

    _guard_report_inputs(rows)
    summary = _summary(rows)
    _write_charts(rows, out)
    _write_examples(rows, out / "examples")
    _write_markdown(rows, summary, out / "phase3_handoff_report.md")
    (out / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _guard_report_inputs(rows: Iterable[BenchRow]) -> None:
    for row in rows:
        if row.test_kind == TestKind.PROMPT_RETENTION:
            raise SystemExit(
                "prompt_retention rows are diagnostics only and are not allowed "
                "in the Phase 3 handoff report input.")
        if row.score_status == ScoreStatus.NEEDS_JUDGE and row.decision_score is not None:
            raise SystemExit(
                f"{row.case_id}/{row.condition.value}: needs_judge row carried "
                "a decision_score. Missing judge is never zero.")
        if row.condition == Condition.DPM_PHASE3_CHECKPOINT:
            if row.gate_may_use is None:
                raise SystemExit(f"{row.case_id}: DPM row missing gate_may_use")
            if row.gate_may_use and not row.audit_certificate_id:
                raise SystemExit(f"{row.case_id}: DPM gate accepted without certificate id")


def _matched_cells_filter(rows: list[BenchRow]) -> list[BenchRow]:
    """Drop any row whose (case_id, repeat) cell is not present for ALL
    conditions in `rows`. Without this, stale-escape and quality
    comparisons can have unequal denominators (e.g. DPM emits a
    stale_memory_escape signal for one fixture that the other
    conditions don't, biasing the rate). The 2026-05 review's MEDIUM
    finding on stale-escape denominator parity required this guard."""
    if not rows:
        return rows
    conds_present = {r.condition for r in rows}
    cells: dict[tuple[str, int], set[Condition]] = {}
    for r in rows:
        cells.setdefault((r.case_id, r.repeat), set()).add(r.condition)
    matched = {k for k, v in cells.items() if v >= conds_present}
    return [r for r in rows if (r.case_id, r.repeat) in matched]


def _summary(rows: list[BenchRow]) -> dict:
    # ChartSpec validation gate: the headline quality chart only
    # admits rows with matching budgets within a (case, test_kind,
    # repeat) cell across conditions. Wired in for the 2026-05
    # review's MEDIUM finding on bypassed chart guards.
    quality_rows = PHASE3_HANDOFF_QUALITY_CHART.filter([
        r for r in rows if r.score_status == ScoreStatus.SCORED
        and r.decision_score is not None
    ])
    # Run a matched-cells parity filter so per-condition means are
    # over the same set of cells.
    quality_rows = _matched_cells_filter(quality_rows)
    score_by_condition = {
        condition.value: _aggregate([r.decision_score for r in quality_rows
                                     if r.condition == condition])
        for condition in Condition
    }

    stale_rows = [
        r for r in rows
        if r.test_kind == TestKind.CORRECTION_SAFETY
        and r.stale_memory_escape is not None
    ]
    # Matched-cells parity for stale escape too: if one condition has a
    # signal on a (case, repeat) but another doesn't, drop the cell so
    # rate denominators are comparable.
    stale_rows = _matched_cells_filter(stale_rows)
    stale_escape_by_condition = {
        condition.value: _aggregate([1.0 if r.stale_memory_escape else 0.0
                                     for r in stale_rows
                                     if r.condition == condition])
        for condition in Condition
    }

    dpm_rows = [r for r in rows if r.condition == Condition.DPM_PHASE3_CHECKPOINT]
    audit_gate = {
        "dpm_rows": len(dpm_rows),
        "gate_accept_count": sum(1 for r in dpm_rows if r.gate_may_use is True),
        "gate_refuse_count": sum(1 for r in dpm_rows if r.gate_may_use is False),
        "audit_pass_count": sum(1 for r in dpm_rows if r.audit_pass is True),
        "correction_emitted_count": sum(
            1 for r in dpm_rows
            if r.audit_verdict == AuditVerdict.CORRECTION_EMITTED),
    }

    cost_by_condition = {
        condition.value: {
            "model_calls": _aggregate([float(r.model_calls) for r in _executed_rows(rows, condition)]),
            "wall_ms": _aggregate([float(r.wall_ms) for r in rows
                                   if r.condition == condition
                                   and r.wall_ms > 0]),
            "input_tokens": _aggregate([float(r.input_tokens) for r in rows
                                        if r.condition == condition
                                        and r.input_tokens > 0]),
            "skipped_or_errored_rows": sum(
                1 for r in rows
                if r.condition == condition
                and r.score_status in {
                    ScoreStatus.SKIPPED_CONTEXT_TOO_LARGE,
                    ScoreStatus.ERRORED,
                }),
        }
        for condition in Condition
    }

    return {
        "row_count": len(rows),
        "cases": sorted({r.case_id for r in rows}),
        "quality_score_by_condition": score_by_condition,
        "stale_escape_by_condition": stale_escape_by_condition,
        "audit_gate": audit_gate,
        "cost_by_condition": cost_by_condition,
        "needs_judge_rows": sum(1 for r in rows if r.score_status == ScoreStatus.NEEDS_JUDGE),
        "errored_rows": sum(1 for r in rows if r.score_status == ScoreStatus.ERRORED),
    }


def _per_cell_variance_section(rows: list[BenchRow]) -> list[str]:
    """Per-(case, condition, test_kind) variance, only emitted when at least
    one cell has more than one sample (i.e. the run was --repeat>1). This is
    the run-to-run noise floor — orthogonal to fixture-difficulty variance."""
    cells: dict[tuple, list[float]] = {}
    for r in rows:
        if r.test_kind not in QUALITY_KINDS: continue
        if r.score_status != ScoreStatus.SCORED: continue
        if r.decision_score is None: continue
        key = (r.case_id, r.condition.value, r.test_kind.value)
        cells.setdefault(key, []).append(float(r.decision_score))
    high_variance = [(k, v) for k, v in cells.items() if len(v) >= 2]
    if not high_variance:
        return []
    high_variance.sort(key=lambda kv: (kv[0][0], kv[0][1], kv[0][2]))
    table_rows: list[list[str]] = []
    for (case_id, cond, kind), vals in high_variance:
        n = len(vals)
        m = mean(vals)
        sd = stdev(vals) if n >= 2 else 0.0
        table_rows.append([case_id, cond, kind, str(n),
                           _fmt(m), _fmt(sd),
                           _fmt(min(vals)), _fmt(max(vals))])
    return [
        "### Per-cell variance (`--repeat > 1`)",
        "",
        "Run-to-run decision_score variance for cells with multiple samples.",
        "High stddev on a cell means Opus stochasticity (no `temperature=0`)",
        "is dominating the signal there; widen the rubric or run more repeats",
        "to get a usable mean.",
        "",
        _markdown_table(
            ["case_id", "condition", "test_kind", "n", "mean", "stddev", "min", "max"],
            table_rows,
        ),
        "",
    ]


def _aggregate(values: Iterable[float | None]) -> dict:
    nums = [float(v) for v in values if v is not None]
    if not nums:
        return {"count": 0, "mean": None, "stddev": None, "min": None, "max": None}
    n = len(nums)
    m = mean(nums)
    sd = stdev(nums) if n >= 2 else 0.0
    return {
        "count": n,
        "mean": m,
        "stddev": sd,
        "min": min(nums),
        "max": max(nums),
    }


def _executed_rows(rows: Iterable[BenchRow], condition: Condition) -> list[BenchRow]:
    return [
        r for r in rows
        if r.condition == condition
        and r.score_status not in {
            ScoreStatus.SKIPPED_CONTEXT_TOO_LARGE,
            ScoreStatus.ERRORED,
        }
        and r.model_calls > 0
    ]


def _write_charts(rows: list[BenchRow], out: Path) -> None:
    quality = _summary(rows)["quality_score_by_condition"]
    write_bar_chart(
        out / "chart_decision_accuracy.svg",
        "Decision Quality",
        _bars_from_aggregate(quality, max_value=1.0),
        value_label="Mean deterministic decision score; judge-only rows excluded",
        max_value=1.0,
    )

    stale = _summary(rows)["stale_escape_by_condition"]
    write_bar_chart(
        out / "chart_stale_memory_escape.svg",
        "Stale-Memory Escape Rate",
        _bars_from_aggregate(stale, max_value=1.0),
        value_label="Lower is better; correction-safety rows only",
        max_value=1.0,
    )

    dpm_rows = [r for r in rows if r.condition == Condition.DPM_PHASE3_CHECKPOINT]
    audit_bars = [
        Bar("DPM gate accepted", _rate(dpm_rows, lambda r: r.gate_may_use is True),
            condition_color(Condition.DPM_PHASE3_CHECKPOINT.value),
            f"{sum(1 for r in dpm_rows if r.gate_may_use is True)}/{len(dpm_rows)} rows"),
        Bar("DPM gate refused", _rate(dpm_rows, lambda r: r.gate_may_use is False),
            "#dc2626",
            f"{sum(1 for r in dpm_rows if r.gate_may_use is False)}/{len(dpm_rows)} rows"),
        Bar("DPM audit pass", _rate(dpm_rows, lambda r: r.audit_pass is True),
            "#0f766e",
            f"{sum(1 for r in dpm_rows if r.audit_pass is True)}/{len(dpm_rows)} rows"),
    ]
    write_bar_chart(
        out / "chart_audit_gate.svg",
        "DPM Audit Gate",
        audit_bars,
        value_label="Share of DPM rows; rolling memory has no comparable primitive",
        max_value=1.0,
    )

    cost = _summary(rows)["cost_by_condition"]
    cost_bars = [
        Bar(condition, stats["model_calls"]["mean"] or 0.0,
            condition_color(condition),
            f"{stats['model_calls']['count']} rows")
        for condition, stats in cost.items()
    ]
    write_bar_chart(
        out / "chart_cost_latency.svg",
        "Model Calls",
        cost_bars,
        value_label="Mean calls per row; rolling memory pays N summary updates",
    )


def _bars_from_aggregate(data: dict, *, max_value: float) -> list[Bar]:
    return [
        Bar(condition, stats["mean"] if stats["mean"] is not None else 0.0,
            condition_color(condition),
            f"{stats['count']} scored rows")
        for condition, stats in data.items()
    ]


def _rate(rows: list[BenchRow], predicate) -> float:
    if not rows:
        return 0.0
    return sum(1 for r in rows if predicate(r)) / len(rows)


def _write_examples(rows: list[BenchRow], out: Path) -> None:
    rolling = next(
        (r for r in rows
         if r.condition == Condition.ROLLING_SUMMARY
         and r.stale_memory_escape is True),
        None,
    )
    dpm_refusal = next(
        (r for r in rows
         if r.condition == Condition.DPM_PHASE3_CHECKPOINT
         and r.gate_may_use is False),
        None,
    )
    dpm_pass = next(
        (r for r in rows
         if r.condition == Condition.DPM_PHASE3_CHECKPOINT
         and r.gate_may_use is True),
        None,
    )

    (out / "rolling_escape_case.md").write_text(
        _rolling_example(rolling), encoding="utf-8")
    (out / "dpm_gate_case.md").write_text(
        _dpm_example(dpm_refusal or dpm_pass), encoding="utf-8")


def _rolling_example(row: BenchRow | None) -> str:
    if row is None:
        return "# Rolling Memory Escape Case\n\nNo stale-memory escape row was present in this run.\n"
    return f"""# Rolling Memory Escape Case

Case: `{row.case_id}`

Condition: `{row.condition.value}`

Budget: `{row.budget_chars}` chars

Decision score: `{row.decision_score}`

Stale-memory escape: `{row.stale_memory_escape}`

Why this matters:

Rolling memory produced a plausible final summary, but the row was marked as a
stale-memory escape. In Phase 3 terms, this is the failure mode DPM is designed
to prevent: there is no checkpoint certificate, event range, or correction gate
that can revoke the stale compressed state before the next decision.

Notes:

{row.notes or "(none)"}
"""


def _dpm_example(row: BenchRow | None) -> str:
    if row is None:
        return "# DPM Gate Case\n\nNo DPM row was present in this run.\n"
    return f"""# DPM Gate Case

Case: `{row.case_id}`

Condition: `{row.condition.value}`

Budget: `{row.budget_chars}` chars

Gate may use checkpoint: `{row.gate_may_use}`

Audit verdict: `{row.audit_verdict.value}`

Audit certificate id: `{row.audit_certificate_id or "(none)"}`

Checkpoint manifest hash: `{row.checkpoint_manifest_hash or "(none)"}`

Blocking corrections: `{", ".join(row.blocking_corrections) if row.blocking_corrections else "(none)"}`

Gate reason:

{row.gate_reason or "(accepted)"}

Why this matters:

DPM exposes the operational evidence rolling memory cannot: which checkpoint was
eligible for decision memory, what the auditor concluded, and whether a blocking
correction forced the system to fail closed or reproject.

Notes:

{row.notes or "(none)"}
"""


def _write_markdown(rows: list[BenchRow], summary: dict, path: Path) -> None:
    lines = [
        "# Phase 3 Handoff Report",
        "",
        "This report compares rolling memory with DPM Phase 3 checkpointed",
        "decision memory on audit-safe handoff after a correction.",
        "",
        "Panel order: **safety / audit first** (the Phase 3 invariant), then",
        "decision quality, then cost. Quality numbers without the audit context",
        "miss the substrate-level result entirely.",
        "",
        "## Run Summary",
        "",
        f"- Rows: `{summary['row_count']}`",
        f"- Cases: `{len(summary['cases'])}`",
        f"- Needs judge rows: `{summary['needs_judge_rows']}`",
        f"- Errored rows: `{summary['errored_rows']}`",
        "",
        "## Safety / Audit (Phase 3 headline)",
        "",
        "### Audit gate",
        "",
        "Rolling memory has no equivalent to this gate; DPM rows expose certificate",
        "and correction evidence directly. Refuse rows force re-projection from raw",
        "events with typed correction directives — a primitive rolling cannot offer.",
        "",
        _markdown_table(
            ["metric", "value"],
            [[k, str(v)] for k, v in summary["audit_gate"].items()],
        ),
        "",
        "![Audit gate](chart_audit_gate.svg)",
        "",
        "### Stale-memory escape",
        "",
        "Lower is better. Counts cells where an invalidated phrase made it into",
        "memory or answer despite a blocking correction in the log.",
        "",
        _markdown_table(
            ["condition", "rows", "escape_rate"],
            [[condition, str(stats["count"]), _fmt(stats["mean"])]
             for condition, stats in summary["stale_escape_by_condition"].items()],
        ),
        "",
        "![Stale-memory escape](chart_stale_memory_escape.svg)",
        "",
        "## Decision Quality",
        "",
        "Mean decision score by condition. **Read this in conjunction with the",
        "Safety / Audit panel above** — a high quality score is not a Phase 3 win",
        "if it was bought by smuggling invalidated state through memory. With",
        "`--repeat > 1` the stddev column measures variance across all rows in",
        "the condition (fixture difficulty + run-to-run noise combined); see the",
        "per-cell variance breakdown below for the run-to-run component alone.",
        "",
        _markdown_table(
            ["condition", "scored_rows", "mean", "stddev", "min", "max"],
            [[condition, str(stats["count"]),
              _fmt(stats["mean"]), _fmt(stats["stddev"]),
              _fmt(stats["min"]), _fmt(stats["max"])]
             for condition, stats in summary["quality_score_by_condition"].items()],
        ),
        "",
        "![Decision quality](chart_decision_accuracy.svg)",
        "",
        *_per_cell_variance_section(rows),
        "",
        "## Cost",
        "",
        _markdown_table(
            ["condition", "executed_rows", "skipped_or_errored", "mean_model_calls", "mean_wall_ms", "mean_input_tokens"],
            [[condition,
              str(stats["model_calls"]["count"]),
              str(stats["skipped_or_errored_rows"]),
              _fmt(stats["model_calls"]["mean"]),
              _fmt(stats["wall_ms"]["mean"]),
              _fmt(stats["input_tokens"]["mean"])]
             for condition, stats in summary["cost_by_condition"].items()],
        ),
        "",
        "![Cost latency](chart_cost_latency.svg)",
        "",
        "## Examples",
        "",
        "- [Rolling memory stale escape](examples/rolling_escape_case.md)",
        "- [DPM audit gate case](examples/dpm_gate_case.md)",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def _markdown_table(headers: list[str], rows: list[list[str]]) -> str:
    out = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    out.extend("| " + " | ".join(row) + " |" for row in rows)
    return "\n".join(out)


def _fmt(value: float | None) -> str:
    if value is None:
        return "n/a"
    if abs(value) >= 100:
        return f"{value:.0f}"
    return f"{value:.3f}".rstrip("0").rstrip(".")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="Phase 3 bench JSONL rows")
    parser.add_argument("--out_dir", required=True, help="Report output directory")
    args = parser.parse_args()

    rows = load_rows(args.input)
    write_report(rows, args.out_dir)
    print(f"Wrote Phase 3 report to {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
