#!/usr/bin/env python3
# Copyright 2026 Google LLC.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Renders the five DPM Projection Cliff charts from JSONL output.

Order in the writeup is E -> B -> C -> D -> A so a skeptic walks costs
first, mechanism breakdown second, architecture sizing third, the
adversarial audit fourth, and the headline cliff last.

Reads one or more JSONL files (see schema.md). Emits one SVG per chart
with deterministic file names so the docs/blog can embed them without
manual regeneration.

Run with --mock to synthesize plausible JSONL rows for layout review
when the driver is not yet wired.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import os
import pathlib
import random
import sys
from typing import Iterable, List, Sequence

# matplotlib is intentionally a soft dependency: imported inside main so
# `python plot.py --help` works in environments without matplotlib.


@dataclasses.dataclass
class Row:
    schema_version: int
    condition: str
    trajectory_chars: int
    memory_budget_chars: int
    repeat_idx: int
    seed: int
    architecture_tag: str
    manifest_hash: str
    runtime_version: str
    kv_dtype: str
    kv_dtype_policy_replay_safe: bool
    model_class: str
    decision_score: float
    wall_clock_decision_ms: float
    wall_clock_append_p50_us: float
    wall_clock_append_p99_us: float
    disk_bytes_session_total: int
    disk_bytes_event_log: int
    disk_bytes_checkpoint_blobs: int
    wall_clock_checkpoint_put_ms: float | None = None
    wall_clock_thaw_ms: float | None = None
    kv_bytes_per_1024_tokens: int | None = None
    must_refill_from_log: bool | None = None
    tamper_test: dict | None = None
    mock: bool = False


# ----------------------------------------------------------------------
# IO


def load_rows(paths: Iterable[str]) -> List[Row]:
    rows: List[Row] = []
    for path in paths:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                obj = json.loads(line)
                if obj.get("schema_version") != 1:
                    raise ValueError(
                        f"unsupported schema_version in {path}: "
                        f"{obj.get('schema_version')!r}"
                    )
                rows.append(Row(**{**_field_defaults(), **obj}))
    return rows


def _field_defaults() -> dict:
    return {
        "wall_clock_checkpoint_put_ms": None,
        "wall_clock_thaw_ms": None,
        "kv_bytes_per_1024_tokens": None,
        "must_refill_from_log": None,
        "tamper_test": None,
        "mock": False,
    }


# ----------------------------------------------------------------------
# Mock data generator (for layout review).
#
# The point: a viewer should be able to render every chart and judge
# the layout / typography / story before the driver is wired against
# real hardware. Every mock row carries mock=True so production data
# never silently mixes with synthetic shapes.


def synthesize_mock_rows(seed: int = 20260420) -> List[Row]:
    rng = random.Random(seed)
    rows: List[Row] = []

    # Trajectory sweep (Chart A + Chart B contributors).
    conditions = [
        "summarization_baseline",
        "dpm_projection",
        "dpm_checkpoints",
        "dpm_checkpoints_prefix_cached",
    ]
    trajectory_grid = [1000, 5000, 10000, 27000, 50000, 100000, 200000]
    # The cliff: summarization collapses around 27k, others stay flat.
    score_curves = {
        "summarization_baseline":
            lambda t: max(0.05, 0.95 - 0.85 * max(0, (t - 5000)) / 95000),
        "dpm_projection":
            lambda t: 0.93 - 0.04 * max(0, (t - 100000)) / 100000,
        "dpm_checkpoints":
            lambda t: 0.93 - 0.03 * max(0, (t - 100000)) / 100000,
        "dpm_checkpoints_prefix_cached":
            lambda t: 0.93 - 0.02 * max(0, (t - 100000)) / 100000,
    }
    latency_curves = {
        "summarization_baseline": lambda t: 200 + 0.06 * t,
        "dpm_projection": lambda t: 60 + 0.012 * t,
        "dpm_checkpoints": lambda t: 30 + 0.004 * t,
        "dpm_checkpoints_prefix_cached": lambda t: 18 + 0.002 * t,
    }
    for condition in conditions:
        for traj in trajectory_grid:
            for repeat in range(3):
                noise = rng.uniform(-0.01, 0.01)
                rows.append(_mock_row(
                    condition=condition,
                    trajectory_chars=traj,
                    memory_budget_chars=5352,
                    repeat_idx=repeat,
                    decision_score=max(0.0, min(
                        1.0, score_curves[condition](traj) + noise)),
                    wall_clock_decision_ms=latency_curves[condition](traj)
                    + rng.uniform(-5, 5),
                ))

    # Costs sweep (Chart E). One short trajectory, two conditions, more repeats.
    for condition in ["dpm_projection", "dpm_checkpoints"]:
        for repeat in range(20):
            checkpoint_put = rng.uniform(110, 140) if condition == "dpm_checkpoints" else None
            rows.append(_mock_row(
                condition=condition,
                trajectory_chars=27000,
                memory_budget_chars=5352,
                repeat_idx=repeat,
                wall_clock_append_p50_us=rng.uniform(40, 70),
                wall_clock_append_p99_us=rng.uniform(900, 1400),
                wall_clock_checkpoint_put_ms=checkpoint_put,
                disk_bytes_session_total=180_000 + (
                    20_000_000 if condition == "dpm_checkpoints" else 0),
                disk_bytes_event_log=180_000,
                disk_bytes_checkpoint_blobs=(
                    20_000_000 if condition == "dpm_checkpoints" else 0),
            ))

    # Architecture sweep (Chart C). One row per (model_class, dtype).
    arch_kv_per_1024 = {
        ("dense_mha", "fp16"):       2_400_000,
        ("dense_mha", "int8_per_token"): 1_200_000,
        ("gqa", "fp16"):             600_000,
        ("gqa", "int8_per_token"):   300_000,
        ("mqa", "fp16"):             300_000,
        ("mqa", "int8_per_token"):   150_000,
        ("mla", "fp16"):             240_000,
        ("mla", "int8_per_token"):   120_000,
        ("sliding_window", "fp16"):  150_000,
        ("sliding_window", "int8_per_token"): 75_000,
    }
    for (mc, dt), bytes_per_1024 in arch_kv_per_1024.items():
        rows.append(_mock_row(
            condition="dpm_projection",
            trajectory_chars=4096,
            memory_budget_chars=5352,
            repeat_idx=0,
            model_class=mc,
            kv_dtype=dt,
            kv_dtype_policy_replay_safe=(dt == "fp16"),
            kv_bytes_per_1024_tokens=bytes_per_1024,
        ))

    # Adversarial-audit scenarios (Chart D).
    adversarial_scenarios = [
        ("clean", None, None, "checkpoint is thaw-compatible", False),
        ("manifest_hash_mismatch_architecture_tag",
         "producer.architecture_tag", "manifest digest mismatch",
         "manifest digest mismatch", True),
        ("manifest_hash_mismatch_artifact_hash",
         "model.artifact_hash", "manifest digest mismatch",
         "manifest digest mismatch", True),
        ("cross_tenant_inject", "identity.tenant_id",
         "checkpoint store rejected cross-tenant injection",
         "checkpoint store rejected cross-tenant injection", True),
        ("malformed_kv_payload", "body bytes",
         "DPM event log record length is corrupt",
         "DPM event log record length is corrupt", True),
    ]
    for scenario, field, reason, _, refill in adversarial_scenarios:
        rows.append(_mock_row(
            condition="dpm_checkpoints",
            trajectory_chars=27000,
            memory_budget_chars=5352,
            repeat_idx=0,
            tamper_test={
                "scenario": scenario,
                "tampered_field": field,
                "expected_manifest_hash": "ab" * 32,
                "actual_manifest_hash": "ab" * 32 if scenario == "clean"
                                                  else "cd" * 32,
                "thaw_decision": "ok" if not refill else "must_refill_from_log",
                "reason": reason if reason else "checkpoint is thaw-compatible",
            },
            must_refill_from_log=refill,
        ))

    return rows


def _mock_row(**overrides) -> Row:
    base = dict(
        schema_version=1,
        condition="dpm_projection",
        trajectory_chars=27000,
        memory_budget_chars=5352,
        repeat_idx=0,
        seed=20260420,
        architecture_tag="mock-rig",
        manifest_hash="ab" * 32,
        runtime_version="litertlm-mock",
        kv_dtype="fp16",
        kv_dtype_policy_replay_safe=True,
        model_class="gqa",
        decision_score=0.9,
        wall_clock_decision_ms=80.0,
        wall_clock_append_p50_us=50.0,
        wall_clock_append_p99_us=1000.0,
        disk_bytes_session_total=200_000,
        disk_bytes_event_log=180_000,
        disk_bytes_checkpoint_blobs=0,
        mock=True,
    )
    base.update(overrides)
    return Row(**base)


# ----------------------------------------------------------------------
# Charts.
#
# All chart functions take a list[Row] and a pathlib.Path output dir.
# Each function picks the rows it cares about and skips the rest. The
# functions emit a deterministic file name; tests can compare bytes
# between runs.


def chart_e_costs(rows: List[Row], out_dir: pathlib.Path) -> pathlib.Path:
    """Costs first. The skeptic's chart."""
    import matplotlib.pyplot as plt  # local import; soft dep
    import numpy as np

    cost_rows = [r for r in rows if r.repeat_idx >= 0
                 and r.condition in ("dpm_projection", "dpm_checkpoints")
                 and r.trajectory_chars == 27000]
    if not cost_rows:
        raise SystemExit("chart E: no rows for costs sweep")

    by_condition = {}
    for r in cost_rows:
        by_condition.setdefault(r.condition, []).append(r)

    metrics = [
        ("Append p50 (µs)",
         lambda rs: float(np.median([r.wall_clock_append_p50_us for r in rs]))),
        ("Append p99 (µs)",
         lambda rs: float(np.median([r.wall_clock_append_p99_us for r in rs]))),
        ("Checkpoint Put (ms)",
         lambda rs: float(np.median([r.wall_clock_checkpoint_put_ms
                                      for r in rs
                                      if r.wall_clock_checkpoint_put_ms]) or 0)),
        ("Disk per session (KB)",
         lambda rs: float(np.median([r.disk_bytes_session_total / 1024.0
                                      for r in rs]))),
    ]
    conditions = list(by_condition.keys())
    fig, ax = plt.subplots(figsize=(9, 4.5))
    bar_w = 0.35
    x = np.arange(len(metrics))
    for i, cond in enumerate(conditions):
        values = [m[1](by_condition[cond]) for m in metrics]
        ax.bar(x + (i - 0.5) * bar_w, values, bar_w, label=cond)
    ax.set_xticks(x)
    ax.set_xticklabels([m[0] for m in metrics])
    ax.set_yscale("log")
    ax.set_ylabel("value (log)")
    ax.set_title("Chart E — Costs first (mock data)" if any(r.mock for r in cost_rows)
                 else "Chart E — Costs first")
    ax.legend()
    fig.tight_layout()
    out = out_dir / "chart_e_costs.svg"
    fig.savefig(out)
    plt.close(fig)
    return out


def chart_b_mechanism(rows: List[Row], out_dir: pathlib.Path) -> pathlib.Path:
    """Mechanism breakdown — each architectural decision's contribution."""
    import matplotlib.pyplot as plt
    import numpy as np

    # Use the 27k / 5352 cell to anchor the breakdown.
    cell = [r for r in rows if r.trajectory_chars == 27000
                              and r.memory_budget_chars == 5352]
    if not cell:
        raise SystemExit("chart B: no rows at 27000 chars / 5352 budget")
    by_cond = {}
    for r in cell:
        by_cond.setdefault(r.condition, []).append(r.decision_score)

    order = [
        ("summarization_baseline", "Stateless\nsummarization"),
        ("dpm_projection",         "+ single-call\nprojection"),
        ("dpm_checkpoints",        "+ Phase 2\ncheckpoints"),
        ("dpm_checkpoints_prefix_cached", "+ prefix-cached\nprojection"),
    ]
    means = [float(np.mean(by_cond.get(k, [float("nan")]))) for k, _ in order]
    fig, ax = plt.subplots(figsize=(9, 4.5))
    xs = np.arange(len(order))
    bars = ax.bar(xs, means, color=["#888", "#4477aa", "#228833", "#aa3377"])
    for i, m in enumerate(means):
        ax.text(i, m + 0.01, f"{m:.2f}", ha="center", va="bottom",
                fontsize=10)
        if i > 0 and not math.isnan(means[i - 1]):
            delta = m - means[i - 1]
            ax.text(i - 0.5, max(means[i], means[i - 1]) + 0.05,
                    f"{delta:+.2f}", ha="center", va="bottom",
                    fontsize=11, color="#aa3377")
    ax.set_xticks(xs)
    ax.set_xticklabels([label for _, label in order])
    ax.set_ylim(0, 1.05)
    ax.set_ylabel("Decision-alignment score (FRP × RCS × CRR × EDA)")
    ax.set_title("Chart B — Where the win comes from (mock data)" if any(r.mock for r in cell)
                 else "Chart B — Where the win comes from")
    fig.tight_layout()
    out = out_dir / "chart_b_mechanism.svg"
    fig.savefig(out)
    plt.close(fig)
    return out


def chart_c_architecture(rows: List[Row], out_dir: pathlib.Path) -> pathlib.Path:
    """Checkpoint size by architecture, colored by replay-safety."""
    import matplotlib.pyplot as plt
    import numpy as np

    arch_rows = [r for r in rows if r.kv_bytes_per_1024_tokens is not None]
    if not arch_rows:
        raise SystemExit("chart C: no rows with kv_bytes_per_1024_tokens")
    classes_in_order = ["dense_mha", "gqa", "mqa", "mla", "sliding_window"]
    fp16_vals = [next(
        (r.kv_bytes_per_1024_tokens for r in arch_rows
         if r.model_class == c and r.kv_dtype == "fp16"), 0)
        for c in classes_in_order]
    int8_vals = [next(
        (r.kv_bytes_per_1024_tokens for r in arch_rows
         if r.model_class == c and r.kv_dtype == "int8_per_token"), 0)
        for c in classes_in_order]
    fig, ax = plt.subplots(figsize=(9, 4.5))
    xs = np.arange(len(classes_in_order))
    ax.bar(xs - 0.2, fp16_vals, 0.4, label="fp16 (replay-safe)",
           color="#228833")
    ax.bar(xs + 0.2, int8_vals, 0.4,
           label="int8-per-token (lossy, policy-gated)", color="#dd8855")
    # The TCP MTU reference line.
    ax.axhline(1500, color="#444", linewidth=0.8, linestyle="--",
               label="single TCP MTU (1500 B)")
    ax.set_xticks(xs)
    ax.set_xticklabels(classes_in_order, rotation=0)
    ax.set_yscale("log")
    ax.set_ylabel("KV bytes per 1024 tokens (log)")
    ax.set_title("Chart C — Checkpoint size by architecture (mock data)"
                 if any(r.mock for r in arch_rows)
                 else "Chart C — Checkpoint size by architecture")
    ax.legend(loc="upper right")
    fig.tight_layout()
    out = out_dir / "chart_c_architecture.svg"
    fig.savefig(out)
    plt.close(fig)
    return out


def chart_d_adversarial(rows: List[Row],
                        out_dir: pathlib.Path) -> pathlib.Path:
    """Adversarial audit — tampered manifest detected by the runtime."""
    import matplotlib.pyplot as plt

    tamper_rows = [r for r in rows if r.tamper_test]
    if not tamper_rows:
        raise SystemExit("chart D: no tamper_test rows")
    fig, ax = plt.subplots(figsize=(9, 4.5))
    scenarios = [r.tamper_test["scenario"] for r in tamper_rows]
    decisions = [r.tamper_test["thaw_decision"] for r in tamper_rows]
    colors = ["#228833" if d == "ok" else "#cc3311" for d in decisions]
    ax.barh(scenarios, [1.0] * len(scenarios), color=colors)
    for i, r in enumerate(tamper_rows):
        ax.text(0.02, i, r.tamper_test["reason"], color="white",
                va="center", fontsize=9)
        if r.tamper_test.get("tampered_field"):
            ax.text(0.98, i, "field: " + r.tamper_test["tampered_field"],
                    color="white", va="center", ha="right", fontsize=8,
                    style="italic")
    ax.set_xlim(0, 1)
    ax.set_xticks([])
    ax.set_title("Chart D — Adversarial audit (mock data)"
                 if any(r.mock for r in tamper_rows)
                 else "Chart D — Adversarial audit")
    fig.tight_layout()
    out = out_dir / "chart_d_adversarial.svg"
    fig.savefig(out)
    plt.close(fig)
    return out


def chart_a_cliff(rows: List[Row], out_dir: pathlib.Path) -> pathlib.Path:
    """The headline: the projection cliff."""
    import matplotlib.pyplot as plt
    import numpy as np

    sweep_rows = [r for r in rows if r.repeat_idx >= 0]
    if not sweep_rows:
        raise SystemExit("chart A: no rows")

    fig, ax_score = plt.subplots(figsize=(10, 5))
    ax_lat = ax_score.twinx()
    ax_lat.set_yscale("log")
    conditions = [
        "summarization_baseline",
        "dpm_projection",
        "dpm_checkpoints",
        "dpm_checkpoints_prefix_cached",
    ]
    palette = {
        "summarization_baseline":           "#aa3377",
        "dpm_projection":                   "#4477aa",
        "dpm_checkpoints":                  "#228833",
        "dpm_checkpoints_prefix_cached":    "#ee7733",
    }
    for cond in conditions:
        cond_rows = sorted(
            [r for r in sweep_rows if r.condition == cond],
            key=lambda r: r.trajectory_chars)
        if not cond_rows:
            continue
        # Mean across repeats, per trajectory.
        by_traj: dict = {}
        for r in cond_rows:
            by_traj.setdefault(r.trajectory_chars, []).append(r)
        xs = sorted(by_traj.keys())
        scores = [float(np.mean([r.decision_score for r in by_traj[t]]))
                  for t in xs]
        latencies = [float(np.mean([r.wall_clock_decision_ms
                                     for r in by_traj[t]])) for t in xs]
        ax_score.plot(xs, scores, marker="o", color=palette[cond],
                      label=f"{cond} (score)")
        ax_lat.plot(xs, latencies, marker="x", color=palette[cond],
                    linestyle=":", alpha=0.7,
                    label=f"{cond} (latency)")
    ax_score.set_xlabel("Trajectory length (characters)")
    ax_score.set_ylabel("Decision-alignment score")
    ax_lat.set_ylabel("Wall-clock per decision (ms, log)")
    ax_score.set_ylim(0, 1.05)
    ax_score.set_title("Chart A — The projection cliff (mock data)"
                       if any(r.mock for r in sweep_rows)
                       else "Chart A — The projection cliff")
    h1, l1 = ax_score.get_legend_handles_labels()
    h2, l2 = ax_lat.get_legend_handles_labels()
    ax_score.legend(h1 + h2, l1 + l2, loc="lower left", fontsize=8)
    fig.tight_layout()
    out = out_dir / "chart_a_cliff.svg"
    fig.savefig(out)
    plt.close(fig)
    return out


# ----------------------------------------------------------------------
# CLI


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input", nargs="*",
        help="One or more JSONL files written by the bench driver. "
             "Required unless --mock is set.")
    parser.add_argument(
        "--output_dir", required=True,
        help="Directory to write the SVG charts to.")
    parser.add_argument(
        "--mock", action="store_true",
        help="Synthesize plausible JSONL rows so the charts can be "
             "rendered before the driver is wired against real hardware.")
    parser.add_argument(
        "--charts", nargs="*", default=["E", "B", "C", "D", "A"],
        help="Subset of charts to render. Default: all five, in skeptic-"
             "resistant order.")
    args = parser.parse_args(argv)

    out_dir = pathlib.Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.mock:
        rows = synthesize_mock_rows()
    else:
        if not args.input:
            parser.error("--input is required unless --mock is set.")
        rows = load_rows(args.input)

    chart_fns = {
        "A": chart_a_cliff,
        "B": chart_b_mechanism,
        "C": chart_c_architecture,
        "D": chart_d_adversarial,
        "E": chart_e_costs,
    }
    for c in args.charts:
        if c not in chart_fns:
            parser.error(f"unknown chart {c!r}; expected one of "
                         f"{list(chart_fns)}")
    written = []
    for c in args.charts:
        try:
            written.append(chart_fns[c](rows, out_dir))
        except SystemExit as e:
            print(f"chart {c}: skipped ({e})", file=sys.stderr)
    print(f"wrote {len(written)} chart(s) to {out_dir}", file=sys.stderr)
    for p in written:
        print(p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
