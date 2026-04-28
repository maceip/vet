#!/usr/bin/env node
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
// G6-style hierarchical DAG of the DPM checkpoint + handoff lifecycle.
//
// What this draws:
//
//   ┌──────────────┐   append      ┌──────────────┐
//   │  Event log   │──────────────▶│  Projector   │
//   │  (raw, S3)   │               │  (DPM call)  │
//   └──────────────┘               └──────┬───────┘
//                                          │ projected memory
//                                          ▼
//                                   ┌──────────────┐
//                                   │ CheckpointStore │
//                                   │ (S3 Express)    │──── PUT N B ───▶ S3
//                                   └──────┬──────────┘
//                                          │ manifest hash
//                                          ▼
//                                   ┌──────────────┐
//                                   │   Decision    │── audit trail ──▶ Postgres
//                                   │  (LLM call)   │
//                                   └──────┬────────┘
//                                          │ thaw
//                                          ▼
//                                   ┌──────────────┐
//                                   │  Fresh node  │
//                                   │  resumes     │
//                                   └──────────────┘
//
// We don't actually invoke @antv/g6 from headless Node here — G6 is
// browser-first and SSR support is fragile. Instead we hand-render the
// same shape into inline-safe SVG using the row data for the byte/ms
// labels. That keeps the output GitHub-renderable without a browser
// dependency, while leaving the option open to swap in a G6 client
// renderer later (interactive_render.mjs is where that lives).

import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import path from "node:path";

import { loadRows, mean } from "./jsonl_loader.mjs";

function parseArgs(argv) {
  const args = { input: [], output_dir: null, allow_mock: false };
  for (let i = 0; i < argv.length; ++i) {
    const a = argv[i];
    if (a === "--input") args.input.push(argv[++i]);
    else if (a === "--output_dir") args.output_dir = argv[++i];
    else if (a === "--allow_mock") args.allow_mock = true;
    else if (a === "--help" || a === "-h") {
      console.log(
        "node dag_render.mjs --input <jsonl> --output_dir <dir> [--allow_mock]",
      );
      process.exit(0);
    } else throw new Error(`unknown arg ${a}`);
  }
  if (args.input.length === 0) throw new Error("--input is required");
  if (!args.output_dir) throw new Error("--output_dir is required");
  return args;
}

function meanOrNull(rows, pick) {
  const xs = rows
    .map(pick)
    .filter((v) => typeof v === "number" && !Number.isNaN(v));
  return xs.length === 0 ? null : xs.reduce((a, b) => a + b, 0) / xs.length;
}

function fmtMs(v) {
  return v == null ? "—" : `${v.toFixed(0)} ms`;
}
function fmtBytes(v) {
  if (v == null) return "—";
  if (v < 1024) return `${v.toFixed(0)} B`;
  if (v < 1024 * 1024) return `${(v / 1024).toFixed(1)} kB`;
  return `${(v / (1024 * 1024)).toFixed(1)} MB`;
}

function renderSvg({
  meanProjectionMs,
  meanCheckpointPutMs,
  meanThawMs,
  meanBytesUploaded,
  meanBytesDownloaded,
  endpoint,
}) {
  // Layout: 7 nodes vertically arranged with two side branches.
  // Width 960, height 720. Hand-tuned coordinates, plain SVG.
  const W = 960;
  const H = 720;
  const nodeW = 220;
  const nodeH = 64;
  const cx = W / 2;
  const node = (x, y, label, sub, fill = "#f4f6fa", border = "#1f2328") => `
    <g transform="translate(${x - nodeW / 2} ${y - nodeH / 2})">
      <rect width="${nodeW}" height="${nodeH}" rx="8" ry="8"
            fill="${fill}" stroke="${border}" stroke-width="1.5"/>
      <text x="${nodeW / 2}" y="24" text-anchor="middle"
            font-family="Inter, -apple-system, sans-serif"
            font-weight="600" font-size="14" fill="#1f2328">${label}</text>
      <text x="${nodeW / 2}" y="46" text-anchor="middle"
            font-family="Inter, -apple-system, sans-serif"
            font-size="11" fill="#57606a">${sub}</text>
    </g>`;
  const arrow = (x1, y1, x2, y2, label = "") => {
    const lx = (x1 + x2) / 2;
    const ly = (y1 + y2) / 2 - 6;
    return `
      <line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}"
            stroke="#1f2328" stroke-width="1.5"
            marker-end="url(#arrow)"/>
      ${label ? `<text x="${lx}" y="${ly}" text-anchor="middle"
            font-family="Inter, -apple-system, sans-serif"
            font-size="11" fill="#57606a">${label}</text>` : ""}`;
  };

  const yLog = 90;
  const yProj = 200;
  const yStore = 320;
  const yDec = 440;
  const yResume = 560;

  const xS3 = cx + 280;
  const xPg = cx + 280;

  return `<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${W} ${H}" width="${W}" height="${H}">
  <defs>
    <marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5"
            markerUnits="strokeWidth" markerWidth="6" markerHeight="6"
            orient="auto">
      <path d="M 0 0 L 10 5 L 0 10 z" fill="#1f2328"/>
    </marker>
  </defs>
  <text x="20" y="32"
        font-family="Inter, -apple-system, sans-serif"
        font-weight="600" font-size="18" fill="#1f2328">
    DPM checkpoint + handoff lifecycle
  </text>
  <text x="20" y="54"
        font-family="Inter, -apple-system, sans-serif"
        font-style="italic" font-size="12" fill="#6e7781">
    Per-decision wall-clock and byte counts measured against
    ${endpoint || "(unknown S3 endpoint)"}.
  </text>

  ${node(cx, yLog, "Event log", "Append-only · S3 Object Lock", "#e8eef7")}
  ${arrow(cx, yLog + nodeH / 2, cx, yProj - nodeH / 2,
          "raw events")}

  ${node(cx, yProj, "DPM Projector",
         `1 LLM call · ~${fmtMs(meanProjectionMs)}`, "#dde7f5")}
  ${arrow(cx, yProj + nodeH / 2, cx, yStore - nodeH / 2,
          "projected memory")}

  ${node(cx, yStore, "CheckpointStore",
         `Put · ${fmtMs(meanCheckpointPutMs)}`, "#d8efe2")}

  ${arrow(cx + nodeW / 2, yStore, xS3 - nodeW / 2, yStore,
          `${fmtBytes(meanBytesUploaded)} ↑`)}
  ${node(xS3, yStore, "S3 Express", "Stockholm · single-AZ", "#cfe6d8")}

  ${arrow(cx, yStore + nodeH / 2, cx, yDec - nodeH / 2,
          "manifest hash")}

  ${node(cx, yDec, "Decision",
         "1 LLM call · projected memory only", "#f5e7d8")}

  ${arrow(cx + nodeW / 2, yDec, xPg - nodeW / 2, yDec,
          "audit row")}
  ${node(xPg, yDec, "Audit ledger", "Postgres · signed certificate",
         "#eedccd")}

  ${arrow(cx, yDec + nodeH / 2, cx, yResume - nodeH / 2,
          `thaw · ${fmtMs(meanThawMs)} · ${fmtBytes(meanBytesDownloaded)} ↓`)}

  ${node(cx, yResume, "Fresh node resumes",
         "Stateless replay from log + manifest", "#f7e6f2")}

  <text x="${W - 20}" y="${H - 16}" text-anchor="end"
        font-family="Inter, -apple-system, sans-serif"
        font-size="10" fill="#9ea7b1">
    DPM cliff bench · checkpoint substrate
  </text>
</svg>
`;
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  mkdirSync(args.output_dir, { recursive: true });
  const rows = loadRows(args.input, { allowMock: args.allow_mock });

  const dpm = rows.filter(
    (r) =>
      r.condition === "dpm_projection" ||
      r.condition === "dpm_checkpoints" ||
      r.condition === "dpm_checkpoints_prefix_cached",
  );
  const ckpt = rows.filter(
    (r) =>
      (r.condition === "dpm_checkpoints" ||
        r.condition === "dpm_checkpoints_prefix_cached") &&
      typeof r.network_bytes_uploaded === "number" &&
      r.network_bytes_uploaded > 0,
  );
  const thaw = rows.filter(
    (r) => r.condition === "dpm_checkpoints_prefix_cached",
  );

  const svg = renderSvg({
    meanProjectionMs: meanOrNull(dpm, (r) => r.wall_clock_decision_ms),
    meanCheckpointPutMs: meanOrNull(
      ckpt,
      (r) => r.wall_clock_checkpoint_put_ms,
    ),
    meanThawMs: meanOrNull(thaw, (r) => r.wall_clock_thaw_ms),
    meanBytesUploaded: meanOrNull(ckpt, (r) => r.network_bytes_uploaded),
    meanBytesDownloaded: meanOrNull(thaw, (r) => r.network_bytes_downloaded),
    endpoint: ckpt.find((r) => r.checkpoint_endpoint)?.checkpoint_endpoint,
  });
  const out = path.join(args.output_dir, "checkpoint_handoff.svg");
  writeFileSync(out, svg);
  console.error(`wrote ${out}`);
  console.log(out);
}

try {
  main();
} catch (e) {
  console.error(e.message);
  process.exit(1);
}
