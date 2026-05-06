// Copyright 2026 Google LLC.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// ECharts option specs for the DPM cliff bench deck. The charts are
// framed around what a tech lead asks before adopting a long-horizon
// agent architecture:
//
//   Chart A — "Does the model still get the right answer when memory
//             is structured?" (per-case decision score by condition)
//   Chart B — "Where does the win come from?" (mean score per condition)
//   Chart E — "What does each decision cost me in wall-clock?" (stacked
//             bar of append + decision + checkpoint_put + thaw)
//   Chart F — "What does it cost in bytes / network round-trips for the
//             real S3 backend?" (bytes-uploaded + thaw-ms paired bar)
//
// SSR mode strips JavaScript from the output SVG and inlines computed
// styles, which is what GitHub's sanitizer requires for inline render.

import {
  CONDITION_ORDER,
  CONDITION_PALETTE,
  HANDOFF_KIND_PALETTE,
  compositeScore,
  groupBy,
  mean,
  uniqueSorted,
} from "./jsonl_loader.mjs";

const TITLE_TEXT_STYLE = {
  fontFamily:
    "Inter, -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif",
  fontWeight: 600,
  fontSize: 16,
  color: "#1f2328",
};

const AXIS_LABEL = {
  fontFamily:
    "Inter, -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif",
  fontSize: 12,
  color: "#57606a",
};

const SUBTITLE = {
  fontFamily:
    "Inter, -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif",
  fontSize: 11,
  color: "#6e7781",
  fontStyle: "italic",
};

function mockSuffix(rows) {
  return rows.some((r) => r.mock) ? " (mock)" : "";
}

// Conditions present in `rows`, in the canonical order.
function presentConditions(rows) {
  const seen = new Set(rows.map((r) => r.condition));
  return CONDITION_ORDER.filter((c) => seen.has(c));
}

// Mean of `pick(row)` across rows in a condition group, ignoring
// undefined / NaN. Returns 0 when the group is empty (so a stacked
// bar still renders at zero rather than dropping the segment).
function meanBy(rows, pick) {
  const xs = rows
    .map(pick)
    .filter((v) => typeof v === "number" && !Number.isNaN(v));
  return xs.length === 0 ? 0 : xs.reduce((a, b) => a + b, 0) / xs.length;
}

// ----------------------------------------------------------------------
// Chart A — Per-case decision score by condition.
// Grouped bar: x = case_id, series = condition, y = decision_score.
// Tells the reader "the model gets case X right under conditions Y, Z."
// Falls back to grouping by trajectory_chars when no case_id is set
// (synthetic-only runs).

export function chartASpec(rows) {
  if (rows.length === 0) throw new Error("chart A: no rows.");
  const groupKey = rows.some((r) => r.case_id) ? "case_id" : "trajectory_chars";
  const groups = uniqueSorted(rows, groupKey);
  const conds = presentConditions(rows);
  const series = conds.map((cond) => ({
    name: cond,
    type: "bar",
    itemStyle: { color: CONDITION_PALETTE[cond] ?? "#888" },
    barMaxWidth: 32,
    label: {
      show: true,
      position: "top",
      formatter: (p) => (p.value == null ? "" : p.value.toFixed(2)),
      ...AXIS_LABEL,
      fontSize: 10,
    },
    data: groups.map((g) => {
      const cell = rows.filter(
        (r) => r.condition === cond && r[groupKey] === g,
      );
      if (cell.length === 0) return null;
      return Number(meanBy(cell, compositeScore).toFixed(3));
    }),
    animationDuration: 800,
  }));
  return {
    title: {
      text: `Chart A — Decision score by ${groupKey === "case_id" ? "case" : "trajectory"} and condition${mockSuffix(rows)}`,
      subtext:
        groupKey === "case_id"
          ? "1.0 = all four probes (FRP, RCS, EDA, CRR) hit; higher is better."
          : "Higher is better. Per-axis mean across runs.",
      textStyle: TITLE_TEXT_STYLE,
      subtextStyle: SUBTITLE,
      left: 16,
      top: 12,
    },
    tooltip: { trigger: "axis", axisPointer: { type: "shadow" } },
    legend: { top: 56, textStyle: AXIS_LABEL, type: "scroll" },
    grid: { left: 56, right: 24, top: 96, bottom: 96 },
    xAxis: {
      type: "category",
      data: groups,
      axisLabel: { ...AXIS_LABEL, rotate: 25, interval: 0 },
    },
    yAxis: {
      type: "value",
      min: 0,
      max: 1.05,
      name: "decision_score",
      nameTextStyle: AXIS_LABEL,
      axisLabel: AXIS_LABEL,
    },
    series,
  };
}

// ----------------------------------------------------------------------
// Chart B — Where the win comes from.
// Single bar series: mean decision_score per condition, aggregated
// across all rows. This is the architectural-progression chart.

export function chartBSpec(rows) {
  if (rows.length === 0) throw new Error("chart B: no rows.");
  const groups = groupBy(rows, "condition");
  const order = [
    ["summarization_baseline", "Stateless\nsummarization"],
    ["dpm_projection", "+ single-call\nprojection"],
    ["dpm_checkpoints", "+ Phase 2\ncheckpoints (S3)"],
    ["dpm_checkpoints_prefix_cached", "+ prefix-cached\nprojection"],
  ];
  const present = order.filter(
    ([cond]) => groups.has(JSON.stringify(cond)),
  );
  const means = present.map(([cond]) => {
    const rs = groups.get(JSON.stringify(cond)) ?? [];
    return meanBy(rs, compositeScore);
  });
  return {
    title: {
      text: `Chart B — Where the win comes from${mockSuffix(rows)}`,
      subtext:
        "Mean decision score per architecture, averaged across all cases.",
      textStyle: TITLE_TEXT_STYLE,
      subtextStyle: SUBTITLE,
      left: 16,
      top: 12,
    },
    tooltip: { trigger: "axis", axisPointer: { type: "shadow" } },
    grid: { left: 60, right: 24, top: 80, bottom: 80 },
    xAxis: {
      type: "category",
      data: present.map(([, label]) => label),
      axisLabel: { ...AXIS_LABEL, lineHeight: 14, interval: 0 },
    },
    yAxis: {
      type: "value",
      min: 0,
      max: 1.05,
      name: "Decision-alignment (FRP × RCS × CRR × EDA)",
      nameTextStyle: AXIS_LABEL,
      axisLabel: AXIS_LABEL,
    },
    series: [
      {
        type: "bar",
        barMaxWidth: 64,
        data: present.map(([cond], i) => ({
          value: Number((means[i] ?? 0).toFixed(3)),
          itemStyle: {
            color: i === 0 ? "#888" : CONDITION_PALETTE[cond] ?? "#888",
          },
        })),
        label: {
          show: true,
          position: "top",
          formatter: (p) => p.value.toFixed(2),
          ...AXIS_LABEL,
        },
        animationDuration: 800,
      },
    ],
  };
}

// ----------------------------------------------------------------------
// Chart E — Per-decision wall-clock breakdown.
// Stacked bar by condition: prefill + decode + checkpoint_put + thaw.
// Answers "where do my milliseconds go?"

export function chartESpec(rows) {
  if (rows.length === 0) throw new Error("chart E: no rows.");
  const conds = presentConditions(rows);
  // Components, in stack order. Each pick(rs) returns mean ms.
  const components = [
    {
      name: "Prefill+decode (ms)",
      color: "#4477aa",
      pick: (rs) => meanBy(rs, (r) => r.wall_clock_decision_ms),
    },
    {
      name: "Checkpoint Put (ms)",
      color: "#228833",
      pick: (rs) => meanBy(rs, (r) => r.wall_clock_checkpoint_put_ms),
    },
    {
      name: "Thaw / cold handoff (ms)",
      color: "#ee7733",
      pick: (rs) => meanBy(rs, (r) => r.wall_clock_thaw_ms),
    },
  ];
  return {
    title: {
      text: `Chart E — Wall-clock per decision${mockSuffix(rows)}`,
      subtext:
        "Stacked: prefill+decode + checkpoint persistence + cold-handoff. " +
        "Smaller is better.",
      textStyle: TITLE_TEXT_STYLE,
      subtextStyle: SUBTITLE,
      left: 16,
      top: 12,
    },
    tooltip: { trigger: "axis", axisPointer: { type: "shadow" } },
    legend: { top: 56, textStyle: AXIS_LABEL, type: "scroll" },
    grid: { left: 80, right: 24, top: 96, bottom: 64 },
    xAxis: {
      type: "category",
      data: conds,
      axisLabel: { ...AXIS_LABEL, rotate: 15, interval: 0 },
    },
    yAxis: {
      type: "value",
      name: "Wall-clock (ms)",
      nameTextStyle: AXIS_LABEL,
      axisLabel: AXIS_LABEL,
    },
    series: components.map((c) => ({
      name: c.name,
      type: "bar",
      stack: "total",
      barMaxWidth: 56,
      itemStyle: { color: c.color },
      data: conds.map((cond) => {
        const rs = rows.filter((r) => r.condition === cond);
        return Number(c.pick(rs).toFixed(1));
      }),
      animationDuration: 800,
    })),
  };
}

// ----------------------------------------------------------------------
// Chart F — Network economics on the S3 substrate.
// Twin-axis bar: mean network_bytes_uploaded (left) + mean thaw_ms (right)
// per condition. Only conditions with non-zero S3 traffic show.
// Answers "what does my real audit trail cost in bytes and ms?"

export function chartFSpec(rows) {
  const s3 = rows.filter(
    (r) =>
      typeof r.network_bytes_uploaded === "number" &&
      r.network_bytes_uploaded > 0,
  );
  if (s3.length === 0) {
    throw new Error(
      "chart F: no rows with network_bytes_uploaded > 0. Run dpm_checkpoints* against --checkpoint_backend=s3_express.",
    );
  }
  const conds = presentConditions(s3);
  const meanUp = (cond) =>
    meanBy(
      s3.filter((r) => r.condition === cond),
      (r) => r.network_bytes_uploaded,
    );
  const meanDown = (cond) =>
    meanBy(
      s3.filter((r) => r.condition === cond),
      (r) => r.network_bytes_downloaded ?? 0,
    );
  const meanPut = (cond) =>
    meanBy(
      s3.filter((r) => r.condition === cond),
      (r) => r.wall_clock_checkpoint_put_ms,
    );
  const meanThaw = (cond) =>
    meanBy(
      s3.filter((r) => r.condition === cond),
      (r) => r.wall_clock_thaw_ms ?? 0,
    );
  const endpoint =
    s3.find((r) => r.checkpoint_endpoint)?.checkpoint_endpoint ??
    "(unknown S3 endpoint)";
  return {
    title: {
      text: `Chart F — Audit-trail and handoff economics${mockSuffix(s3)}`,
      subtext: `S3 endpoint: ${endpoint}`,
      textStyle: TITLE_TEXT_STYLE,
      subtextStyle: SUBTITLE,
      left: 16,
      top: 12,
    },
    tooltip: { trigger: "axis", axisPointer: { type: "shadow" } },
    legend: { top: 56, textStyle: AXIS_LABEL, type: "scroll" },
    grid: { left: 80, right: 80, top: 96, bottom: 64 },
    xAxis: {
      type: "category",
      data: conds,
      axisLabel: { ...AXIS_LABEL, rotate: 15, interval: 0 },
    },
    yAxis: [
      {
        type: "value",
        name: "Bytes per decision",
        nameTextStyle: AXIS_LABEL,
        axisLabel: AXIS_LABEL,
      },
      {
        type: "value",
        name: "ms per round-trip",
        nameTextStyle: AXIS_LABEL,
        axisLabel: AXIS_LABEL,
      },
    ],
    series: [
      {
        name: "Uploaded (bytes)",
        type: "bar",
        yAxisIndex: 0,
        barMaxWidth: 32,
        itemStyle: { color: "#228833" },
        data: conds.map((c) => Number(meanUp(c).toFixed(0))),
        animationDuration: 800,
      },
      {
        name: "Downloaded on thaw (bytes)",
        type: "bar",
        yAxisIndex: 0,
        barMaxWidth: 32,
        itemStyle: { color: "#88bb44" },
        data: conds.map((c) => Number(meanDown(c).toFixed(0))),
        animationDuration: 800,
      },
      {
        name: "Checkpoint Put (ms)",
        type: "line",
        yAxisIndex: 1,
        symbol: "circle",
        symbolSize: 8,
        lineStyle: { width: 2, color: "#aa3377" },
        itemStyle: { color: "#aa3377" },
        data: conds.map((c) => Number(meanPut(c).toFixed(1))),
        animationDuration: 800,
      },
      {
        name: "Thaw / cold handoff (ms)",
        type: "line",
        yAxisIndex: 1,
        symbol: "diamond",
        symbolSize: 8,
        lineStyle: { width: 2, color: "#ee7733" },
        itemStyle: { color: "#ee7733" },
        data: conds.map((c) => Number(meanThaw(c).toFixed(1))),
        animationDuration: 800,
      },
    ],
  };
}

// ----------------------------------------------------------------------
// Chart D — Handoff fidelity panel (the headline for Phase 2).
// One ECharts spec with two grids stacked vertically:
//   D-top:    per-case bar of cumulative_bytes_uploaded by analyst-A
//             across the trajectory, colored by handoff_kind.
//             Overlaid line: handoff_wall_to_resume_ms (analyst-B's
//             time-to-pick-up) compared against handoff_cold_baseline_wall_ms.
//   D-bottom: 4-row heatmap of boundary outcomes (one row per
//             property, one column per case). Green = blocked-as-
//             expected, red = property held but assertion failed.

const HANDOFF_BUCKETS = [
  { kind: "explicit", label: "explicit handoff_request" },
  { kind: "synthetic_severe_milestone", label: "synthetic @ severe MITRE (T1003/T1021/T1078/T1486)" },
  { kind: "synthetic_milestone", label: "synthetic @ milestone" },
  { kind: "synthetic_median", label: "synthetic @ median position" },
];

const BOUNDARY_PROPERTIES = [
  { key: "cross_tenant_breach_blocked", label: "Cross-tenant breach blocked" },
  { key: "expired_credential_blocked",  label: "Expired credential blocked" },
  { key: "tampered_audit_detected",     label: "Tampered audit detected" },
  { key: "replay_blocked",              label: "Replay blocked" },
];

function handoffRows(rows) {
  return rows.filter(
    (r) => typeof r.handoff_id === "string" && r.handoff_id.length > 0,
  );
}

export function chartDSpec(rows) {
  const ho = handoffRows(rows);
  if (ho.length === 0) {
    throw new Error(
      "chart D: no handoff rows. Run --condition=dpm_checkpoints_handoff " +
      "against --checkpoint_backend=s3_express + the deployed broker.",
    );
  }
  // Sort cases by case_id (or synthesise from trajectory_chars).
  const cases = ho
    .slice()
    .sort((a, b) => String(a.case_id ?? a.trajectory_chars ?? "")
      .localeCompare(String(b.case_id ?? b.trajectory_chars ?? "")));
  const labels = cases.map((r) => r.case_id ?? `traj-${r.trajectory_chars}`);

  // Build one bar series per handoff_kind so the legend doubles as a
  // breakdown of explicit-vs-synthetic coverage.
  const byKind = HANDOFF_BUCKETS.map(({ kind, label }) => ({
    name: label,
    type: "bar",
    stack: "bytes",
    barMaxWidth: 14,
    itemStyle: { color: HANDOFF_KIND_PALETTE[kind] ?? "#888" },
    data: cases.map((r) =>
      r.handoff_kind === kind
        ? Number(((r.handoff_cumulative_bytes_uploaded ?? 0)).toFixed(0))
        : 0,
    ),
    xAxisIndex: 0,
    yAxisIndex: 0,
    emphasis: { focus: "series" },
  }));

  const wallResume = {
    name: "wall_to_resume_ms (analyst-B)",
    type: "line",
    symbol: "circle",
    symbolSize: 6,
    lineStyle: { width: 2, color: "#aa3377" },
    itemStyle: { color: "#aa3377" },
    data: cases.map((r) => r.handoff_wall_to_resume_ms ?? null),
    xAxisIndex: 0,
    yAxisIndex: 1,
    z: 5,
    emphasis: { focus: "series" },
  };
  const wallCold = {
    name: "cold_baseline_wall_ms",
    type: "line",
    symbol: "diamond",
    symbolSize: 6,
    lineStyle: { width: 1.5, color: "#ee7733", type: "dashed" },
    itemStyle: { color: "#ee7733" },
    data: cases.map((r) => r.handoff_cold_baseline_wall_ms ?? null),
    xAxisIndex: 0,
    yAxisIndex: 1,
    z: 5,
    emphasis: { focus: "series" },
  };

  // Heatmap data: each cell is [colIdx, rowIdx, valueOrNull].
  // null means "row didn't carry this boundary field" → grey.
  const heatmapData = [];
  cases.forEach((r, ci) => {
    BOUNDARY_PROPERTIES.forEach(({ key }, ri) => {
      const v = r[key];
      heatmapData.push([
        ci, ri,
        typeof v === "boolean" ? (v ? 1 : 0) : -1,
      ]);
    });
  });

  return {
    title: [
      {
        text: `Chart D — Phase 2 handoff fidelity (${cases.length} cases)`,
        subtext:
          "Top: cumulative S3 bytes that flowed during the trajectory, " +
          "colored by where the harness placed the analyst-A → " +
          "analyst-B handoff (explicit corpus event vs synthetic " +
          "MITRE-anchored). Lines compare analyst-B's wall_to_resume " +
          "vs the cold-baseline cost of fetching the raw event log. " +
          "Bottom: four boundary properties per case — green = " +
          "blocked-as-expected, red = blocked but assertion failed.",
        textStyle: TITLE_TEXT_STYLE,
        subtextStyle: SUBTITLE,
        left: 16,
        top: 12,
      },
    ],
    tooltip: [
      {
        trigger: "axis",
        axisPointer: { type: "shadow" },
        formatter: (items) => {
          const idx = items?.[0]?.dataIndex;
          if (idx == null) return "";
          const r = cases[idx];
          const lines = [
            `<b>${r.case_id ?? "(synthetic)"}</b>`,
            `kind: ${r.handoff_kind ?? "—"}`,
            `event: ${r.handoff_event_index ?? "—"} / ${r.handoff_total_events ?? "—"}`,
            `checkpoints: ${r.handoff_checkpoint_count ?? "—"}`,
            `bytes up: ${r.handoff_cumulative_bytes_uploaded ?? "—"}`,
            `wall PUT: ${
              typeof r.handoff_cumulative_wall_put_ms === "number"
                ? r.handoff_cumulative_wall_put_ms.toFixed(1) + " ms" : "—"}`,
            `wall to resume: ${
              typeof r.handoff_wall_to_resume_ms === "number"
                ? r.handoff_wall_to_resume_ms.toFixed(1) + " ms" : "—"}`,
            `cold baseline: ${
              typeof r.handoff_cold_baseline_wall_ms === "number"
                ? r.handoff_cold_baseline_wall_ms.toFixed(1) + " ms" : "—"}`,
          ];
          return lines.join("<br>");
        },
      },
    ],
    legend: {
      top: 88,
      textStyle: AXIS_LABEL,
      type: "scroll",
      data: [
        ...HANDOFF_BUCKETS.map((b) => b.label),
        "wall_to_resume_ms (analyst-B)",
        "cold_baseline_wall_ms",
      ],
    },
    grid: [
      // Top grid: bars + lines.
      { left: 80, right: 80, top: 130, height: "45%" },
      // Bottom grid: heatmap.
      { left: 80, right: 80, top: "70%", height: "20%" },
    ],
    xAxis: [
      {
        type: "category",
        gridIndex: 0,
        data: labels,
        axisLabel: { ...AXIS_LABEL, rotate: 60, interval: 0, fontSize: 9 },
      },
      {
        type: "category",
        gridIndex: 1,
        data: labels,
        axisLabel: { show: false },
        axisTick: { show: false },
      },
    ],
    yAxis: [
      {
        gridIndex: 0,
        type: "value",
        name: "Cumulative S3 bytes",
        nameTextStyle: AXIS_LABEL,
        axisLabel: AXIS_LABEL,
      },
      {
        gridIndex: 0,
        type: "value",
        name: "ms",
        nameTextStyle: AXIS_LABEL,
        axisLabel: AXIS_LABEL,
        position: "right",
      },
      {
        gridIndex: 1,
        type: "category",
        data: BOUNDARY_PROPERTIES.map((p) => p.label),
        axisLabel: { ...AXIS_LABEL, fontSize: 10 },
        axisTick: { show: false },
      },
    ],
    visualMap: {
      type: "piecewise",
      show: false,
      seriesIndex: byKind.length + 2, // index of the heatmap series
      pieces: [
        { value: 1,  color: "#228833", label: "blocked" },
        { value: 0,  color: "#cc3322", label: "FAILED" },
        { value: -1, color: "#dddddd", label: "n/a" },
      ],
    },
    series: [
      ...byKind,
      wallResume,
      wallCold,
      {
        name: "boundary outcomes",
        type: "heatmap",
        xAxisIndex: 1,
        yAxisIndex: 2,
        data: heatmapData,
        label: { show: false },
        progressive: 0,
        emphasis: { itemStyle: { borderColor: "#000", borderWidth: 1 } },
      },
    ],
  };
}

// Aggregate small-multiples view: per-handoff-kind summary card useful
// at the top of the gh-pages deck.
export function handoffSummary(rows) {
  const ho = handoffRows(rows);
  if (ho.length === 0) return null;
  const kindHist = {};
  for (const r of ho) {
    const k = r.handoff_kind ?? "(unknown)";
    kindHist[k] = (kindHist[k] ?? 0) + 1;
  }
  const boundaryHist = {};
  for (const { key, label } of BOUNDARY_PROPERTIES) {
    let pass = 0, fail = 0, na = 0;
    for (const r of ho) {
      const v = r[key];
      if (typeof v !== "boolean") na++;
      else if (v) pass++;
      else fail++;
    }
    boundaryHist[label] = { pass, fail, na };
  }
  let totalCheckpoints = 0;
  let totalBytes = 0;
  let resumeMs = 0, resumeN = 0;
  let coldMs = 0, coldN = 0;
  for (const r of ho) {
    totalCheckpoints += r.handoff_checkpoint_count ?? 0;
    totalBytes += r.handoff_cumulative_bytes_uploaded ?? 0;
    if (typeof r.handoff_wall_to_resume_ms === "number") {
      resumeMs += r.handoff_wall_to_resume_ms; resumeN++;
    }
    if (typeof r.handoff_cold_baseline_wall_ms === "number") {
      coldMs += r.handoff_cold_baseline_wall_ms; coldN++;
    }
  }
  return {
    cases: ho.length,
    kind_histogram: kindHist,
    boundary_histogram: boundaryHist,
    total_checkpoints: totalCheckpoints,
    total_bytes_uploaded: totalBytes,
    mean_wall_to_resume_ms: resumeN ? resumeMs / resumeN : null,
    mean_cold_baseline_wall_ms: coldN ? coldMs / coldN : null,
  };
}

// ----------------------------------------------------------------------
// Legacy entry points kept for callers (ssr_render.mjs) that still
// reference them. They map to the new specs above.

export function chartECostsSpec(rows) {
  return chartESpec(rows);
}
export function chartBMechanismSpec(rows) {
  return chartBSpec(rows);
}
export function chartCArchitectureSpec(rows) {
  // No architecture sweep yet — point reader at chart F instead.
  return chartFSpec(rows);
}
export function chartACliffSpecs(rows) {
  return [{ key: "chart_a_cliff", spec: chartASpec(rows) }];
}
