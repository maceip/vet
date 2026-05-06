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
// Interactive HTML mirror of the SSR chart deck. Loads ECharts from a
// CDN at runtime, instantiates the same specs we use server-side, and
// inlines the JSONL row data as a JS literal so the page is a single
// file that renders without a server.
//
// Output: <out_dir>/interactive.html
//
// Usage:
//   node interactive_render.mjs --input <jsonl> [--input ...]
//                                --output_dir <dir>
//                                [--allow_mock]

import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import path from "node:path";

import { loadRows } from "./jsonl_loader.mjs";
import {
  chartASpec,
  chartBSpec,
  chartDSpec,
  chartESpec,
  chartFSpec,
  handoffSummary,
} from "./echarts_specs.mjs";

function parseArgs(argv) {
  const args = { input: [], output_dir: null, allow_mock: false };
  for (let i = 0; i < argv.length; ++i) {
    const a = argv[i];
    if (a === "--input") args.input.push(argv[++i]);
    else if (a === "--output_dir") args.output_dir = argv[++i];
    else if (a === "--allow_mock") args.allow_mock = true;
    else if (a === "--help" || a === "-h") {
      console.log(
        "node interactive_render.mjs --input <jsonl> [--input ...] --output_dir <dir>",
      );
      process.exit(0);
    } else throw new Error(`unknown arg ${a}`);
  }
  if (args.input.length === 0) throw new Error("--input is required");
  if (!args.output_dir) throw new Error("--output_dir is required");
  return args;
}

function buildHtml(specsByPanel) {
  const json = JSON.stringify(specsByPanel);
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<title>DPM Projection Cliff — interactive deck</title>
<style>
  body { font-family: Inter, -apple-system, "Segoe UI", Roboto, sans-serif;
         margin: 0; padding: 24px; background: #f6f8fa; color: #1f2328; }
  h1 { font-size: 22px; margin: 0 0 4px; }
  p.lede { color: #57606a; margin: 0 0 24px; max-width: 740px; }
  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
  .card { background: white; border: 1px solid #d0d7de;
          border-radius: 8px; padding: 8px;
          box-shadow: 0 1px 0 rgba(31,35,40,0.04); }
  .card > div.chart { width: 100%; height: 420px; }
  footer { color: #6e7781; font-size: 12px; margin-top: 24px; }
  @media (max-width: 1100px) { .grid { grid-template-columns: 1fr; } }
</style>
</head>
<body>
<h1>DPM Projection Cliff — interactive deck</h1>
<p class="lede">
  Decision-substrate measurements from the DPM cliff bench. Hover for
  values, drag the legend to toggle series, click axis labels to zoom.
  Same specs ship as inline-safe SVG for GitHub markdown
  (<code>ssr_render.mjs</code>); this page is the off-GitHub mirror.
</p>
<div class="card" style="margin-bottom:16px"><div class="chart" id="panel-d" style="height:560px"></div></div>
<div class="grid">
  <div class="card"><div class="chart" id="panel-a"></div></div>
  <div class="card"><div class="chart" id="panel-b"></div></div>
  <div class="card"><div class="chart" id="panel-e"></div></div>
  <div class="card"><div class="chart" id="panel-f"></div></div>
</div>
<footer>
  Rendered from JSONL emitted by <code>dpm_projection_cliff</code>.
  Chart F's S3 endpoint is the actual checkpoint store the bench
  exercised; substrate measurements are wire-real.
</footer>
<script src="https://cdn.jsdelivr.net/npm/echarts@5.5.1/dist/echarts.min.js"></script>
<script>
  const SPECS = ${json};
  const mount = (id, spec) => {
    if (!spec) return;
    const el = document.getElementById(id);
    const inst = echarts.init(el, null, { renderer: "svg" });
    inst.setOption(spec);
    window.addEventListener("resize", () => inst.resize());
  };
  mount("panel-d", SPECS.d);
  mount("panel-a", SPECS.a);
  mount("panel-b", SPECS.b);
  mount("panel-e", SPECS.e);
  mount("panel-f", SPECS.f);
</script>
</body>
</html>
`;
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  mkdirSync(args.output_dir, { recursive: true });
  const rows = loadRows(args.input, { allowMock: args.allow_mock });
  const specs = {};
  try { specs.d = chartDSpec(rows); } catch (e) {
    console.error(`chart D: ${e.message}`);
  }
  try { specs.a = chartASpec(rows); } catch (e) {
    console.error(`chart A: ${e.message}`);
  }
  try { specs.b = chartBSpec(rows); } catch (e) {
    console.error(`chart B: ${e.message}`);
  }
  try { specs.e = chartESpec(rows); } catch (e) {
    console.error(`chart E: ${e.message}`);
  }
  try { specs.f = chartFSpec(rows); } catch (e) {
    console.error(`chart F: ${e.message}`);
  }
  const html = buildHtml(specs);
  const out = path.join(args.output_dir, "interactive.html");
  writeFileSync(out, html);
  console.error(`wrote ${out}`);
  console.log(out);
}

try { main(); } catch (e) {
  console.error(e.message);
  process.exit(1);
}
