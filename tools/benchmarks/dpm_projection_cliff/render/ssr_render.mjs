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

// Server-side ECharts render. Produces inline-safe SVG (no <script>,
// no <iframe>, computed styles inlined) that survives GitHub's
// markdown sanitizer. Use the same JSONL the Python plotter consumes.
//
// Usage:
//   node ssr_render.mjs --input <path>.jsonl [--input ...]
//                       --output_dir <dir>
//                       [--allow_mock]
//                       [--charts E,B,C,A]
//                       [--costs_trajectory 27000]
//                       [--costs_budget 5352]
//                       [--mechanism_trajectory 27000]
//                       [--mechanism_budget 5352]

import { writeFileSync, mkdirSync } from "node:fs";
import path from "node:path";
import * as echarts from "echarts/core";
import { SVGRenderer } from "echarts/renderers";
import {
  BarChart, LineChart, HeatmapChart,
} from "echarts/charts";
import {
  TitleComponent, TooltipComponent, LegendComponent, GridComponent,
  MarkLineComponent, VisualMapComponent,
} from "echarts/components";

import { loadRows } from "./jsonl_loader.mjs";
import {
  chartACliffSpecs,
  chartBMechanismSpec,
  chartCArchitectureSpec,
  chartDSpec,
  chartECostsSpec,
} from "./echarts_specs.mjs";

echarts.use([
  SVGRenderer, BarChart, LineChart, HeatmapChart,
  TitleComponent, TooltipComponent, LegendComponent, GridComponent,
  MarkLineComponent, VisualMapComponent,
]);

function parseArgs(argv) {
  const args = {
    input: [],
    output_dir: null,
    allow_mock: false,
    charts: ["E", "B", "C", "A", "D"],
    costs_trajectory: 27000,
    costs_budget: 5352,
    mechanism_trajectory: 27000,
    mechanism_budget: 5352,
  };
  for (let i = 0; i < argv.length; ++i) {
    const a = argv[i];
    if (a === "--input") args.input.push(argv[++i]);
    else if (a === "--output_dir") args.output_dir = argv[++i];
    else if (a === "--allow_mock") args.allow_mock = true;
    else if (a === "--charts") args.charts = argv[++i].split(",");
    else if (a === "--costs_trajectory") args.costs_trajectory = +argv[++i];
    else if (a === "--costs_budget") args.costs_budget = +argv[++i];
    else if (a === "--mechanism_trajectory") args.mechanism_trajectory = +argv[++i];
    else if (a === "--mechanism_budget") args.mechanism_budget = +argv[++i];
    else if (a === "--help" || a === "-h") {
      console.log(
        "node ssr_render.mjs --input <jsonl> [--input ...] " +
        "--output_dir <dir>");
      process.exit(0);
    } else {
      throw new Error(`unknown arg ${a}`);
    }
  }
  if (args.input.length === 0)
    throw new Error("--input is required.");
  if (!args.output_dir)
    throw new Error("--output_dir is required.");
  return args;
}

function renderSpec(spec, { width = 960, height = 480 } = {}) {
  // ECharts SSR: pass renderer:'svg' and ssr:true. The returned chart
  // emits a self-contained SVG string with no <script> tags.
  const chart = echarts.init(null, null, {
    renderer: "svg",
    ssr: true,
    width,
    height,
  });
  chart.setOption(spec);
  const svg = chart.renderToSVGString();
  chart.dispose();
  return svg;
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  mkdirSync(args.output_dir, { recursive: true });
  const rows = loadRows(args.input, { allowMock: args.allow_mock });

  const written = [];
  const renderers = {
    E: () => [{
      key: "chart_e_costs",
      spec: chartECostsSpec(rows, {
        trajectoryChars: args.costs_trajectory,
        memoryBudgetChars: args.costs_budget,
      }),
    }],
    B: () => [{
      key: "chart_b_mechanism",
      spec: chartBMechanismSpec(rows, {
        trajectoryChars: args.mechanism_trajectory,
        memoryBudgetChars: args.mechanism_budget,
      }),
    }],
    C: () => [{
      key: "chart_c_architecture",
      spec: chartCArchitectureSpec(rows),
    }],
    A: () => chartACliffSpecs(rows),
    D: () => [{
      key: "chart_d_handoff",
      spec: chartDSpec(rows),
    }],
  };
  for (const c of args.charts) {
    const fn = renderers[c];
    if (!fn) {
      console.error(`unknown chart ${c}; skipping`);
      continue;
    }
    let pieces;
    try {
      pieces = fn();
    } catch (e) {
      console.error(`chart ${c}: ${e.message}`);
      continue;
    }
    for (const { key, spec } of pieces) {
      const svg = renderSpec(spec);
      const out = path.join(args.output_dir, `${key}.svg`);
      writeFileSync(out, svg);
      written.push(out);
    }
  }
  console.error(`wrote ${written.length} SVG file(s) to ${args.output_dir}`);
  for (const p of written) console.log(p);
}

try {
  main();
} catch (e) {
  console.error(e.message);
  process.exit(1);
}
