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

import { readFileSync } from "node:fs";

const SCHEMA_VERSION = 1;

// Loads one or more JSONL files written by the bench driver and
// returns a flat array of row objects. Refuses to mix mock=true and
// mock=false rows unless allowMock is true so a stale layout-review
// file cannot silently contaminate a real-hardware chart.
export function loadRows(paths, { allowMock = false } = {}) {
  const rows = [];
  for (const path of paths) {
    const text = readFileSync(path, "utf-8");
    let line_no = 0;
    for (const line of text.split(/\r?\n/)) {
      ++line_no;
      const trimmed = line.trim();
      if (!trimmed) continue;
      let obj;
      try {
        obj = JSON.parse(trimmed);
      } catch (e) {
        throw new Error(`${path}:${line_no}: invalid JSON (${e.message})`);
      }
      if (obj.schema_version !== SCHEMA_VERSION) {
        throw new Error(
          `${path}:${line_no}: unsupported schema_version ` +
          `${JSON.stringify(obj.schema_version)} (expected ${SCHEMA_VERSION})`
        );
      }
      rows.push(obj);
    }
  }
  const hasMock = rows.some((r) => r.mock === true);
  const hasReal = rows.some((r) => r.mock !== true);
  if (hasMock && hasReal && !allowMock) {
    throw new Error(
      "render: input mixes mock=true and mock=false rows. Stale mock " +
      "rows can silently contaminate a real-hardware chart. Re-run " +
      "with --allow_mock to acknowledge, or filter the mock rows out " +
      "of your input first."
    );
  }
  return rows;
}

// Returns the bench-emitted decision_score (mean of FRP/RCS/EDA/CRR
// in [0,1]) when present; otherwise reconstructs the same mean from
// the per-axis fields; otherwise NaN.
//
// Note: an earlier version of this function returned the product
// (FRP × RCS × CRR × EDA), which collapsed to 0 whenever any single
// axis missed and made the bar charts uniformly read 0.00. The bench
// writes decision_score as the per-axis arithmetic mean; charts that
// want the product semantics ("all four axes must hit") should call
// allAxesHit() instead.
export function compositeScore(row) {
  if (typeof row.decision_score === "number") return row.decision_score;
  if (typeof row.deterministic_score === "number") {
    return row.deterministic_score;
  }
  const { frp, rcs, crr, eda } = row;
  if (
    typeof frp === "number" && typeof rcs === "number" &&
    typeof crr === "number" && typeof eda === "number"
  ) {
    return (frp + rcs + crr + eda) / 4;
  }
  return NaN;
}

// Strict "all four axes hit" indicator — 1.0 only when FRP, RCS, EDA,
// CRR are all 1; else 0. Matches the paper's product-of-axes framing.
export function allAxesHit(row) {
  const { frp, rcs, crr, eda } = row;
  if (
    typeof frp !== "number" || typeof rcs !== "number" ||
    typeof crr !== "number" || typeof eda !== "number"
  ) {
    return NaN;
  }
  return frp === 1 && rcs === 1 && crr === 1 && eda === 1 ? 1 : 0;
}

// Returns the unique set of values for `key` across rows, sorted
// ascending (numeric) or lexicographically (string).
export function uniqueSorted(rows, key) {
  const seen = new Set();
  for (const r of rows) seen.add(r[key]);
  const arr = [...seen];
  if (arr.every((v) => typeof v === "number")) arr.sort((a, b) => a - b);
  else arr.sort();
  return arr;
}

// Groups rows by a composite key built from `keys`. Returns Map.
export function groupBy(rows, ...keys) {
  const out = new Map();
  for (const r of rows) {
    const k = keys.map((k) => JSON.stringify(r[k])).join("|");
    if (!out.has(k)) out.set(k, []);
    out.get(k).push(r);
  }
  return out;
}

// Mean across an array of numbers; ignores NaN.
export function mean(values) {
  let sum = 0;
  let n = 0;
  for (const v of values) {
    if (typeof v === "number" && !Number.isNaN(v)) {
      sum += v;
      ++n;
    }
  }
  return n === 0 ? NaN : sum / n;
}

export const CONDITION_ORDER = [
  "rolling_summary_baseline",
  "summarization_baseline",
  "dpm_projection",
  "dpm_checkpoints",
  "dpm_checkpoints_prefix_cached",
  "dpm_checkpoints_handoff",
];

export const CONDITION_PALETTE = {
  rolling_summary_baseline:       "#cc6677",
  summarization_baseline:        "#aa3377",
  dpm_projection:                "#4477aa",
  dpm_checkpoints:               "#228833",
  dpm_checkpoints_prefix_cached: "#ee7733",
  dpm_checkpoints_handoff:       "#0b6ea3",
};

// Color per handoff_kind for Chart D. "explicit" (corpus-tagged) gets
// the strongest color; synthetic ones progressively desaturate so the
// reader can tell at a glance how much of the headline depends on
// curated vs harness-imputed escalation points.
export const HANDOFF_KIND_PALETTE = {
  explicit:                    "#0b6ea3",
  synthetic_severe_milestone:  "#3392c1",
  synthetic_milestone:         "#7fb6d4",
  synthetic_median:            "#bcd6e4",
};

// True when the row carries handoff fields (the dpm_checkpoints_handoff
// condition). Charts A/B/E/F skip handoff rows; Chart D consumes only
// these.
export function isHandoffRow(row) {
  return row.condition === "dpm_checkpoints_handoff" ||
         typeof row.handoff_id === "string";
}
