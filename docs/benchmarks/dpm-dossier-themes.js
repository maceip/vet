const THEME_COPY = {
  nineties: {
    eyebrow: "90s core evidence playground",
    title: "Red-team replay in soft vinyl blocks.",
    deck:
      "A chunky isometric dossier for the DPM benchmark: pastel hazards, playful evidence blocks, and hard labels that still keep the measurement state honest.",
    badge: "Nickelodeon after dark",
    accent: "#ff8b6e",
    good: "#4ecf8f",
    warn: "#ffcc58",
    bad: "#ff6b8a",
  },
  y2k: {
    eyebrow: "Y2K anti-gravity telemetry",
    title: "Assessment velocity / memory pressure.",
    deck:
      "A high-speed interface treatment inspired by late-90s console racing futurism: slashes, warning numerals, acidic contrast, and charts that feel like telemetry.",
    badge: "Wipeout-style telemetry",
    accent: "#f5ff00",
    good: "#00f0ff",
    warn: "#ff8a00",
    bad: "#ff2bd6",
  },
  gilded: {
    eyebrow: "Gilded age audit ledger",
    title: "The red-team assessment, cast in brass.",
    deck:
      "An industrial civic dossier with locomotive brass, ledger paper, geometric ornament, and a Wright/Yamasaki-inspired sense of order and ceremony.",
    badge: "Brass-bound proceedings",
    accent: "#c7943e",
    good: "#2f7d67",
    warn: "#b57721",
    bad: "#9a3f31",
  },
};

const THEME = document.body.dataset.theme || "nineties";
const COPY = THEME_COPY[THEME];
const THEME_STYLES = getComputedStyle(document.body);
const THEME_INK = THEME_STYLES.getPropertyValue("--ink").trim() || "#111119";
const THEME_MUTED = THEME_STYLES.getPropertyValue("--muted").trim() || "#34343f";
const THEME_LINE = THEME_STYLES.getPropertyValue("--line").trim() || "rgba(17, 17, 25, 0.3)";
const THEME_PANEL = THEME_STYLES.getPropertyValue("--panel").trim() || "rgba(255, 255, 255, 0.72)";
const THEME_DISPLAY_FONT =
  THEME_STYLES.getPropertyValue("--font-display").trim() || 'Impact, Haettenschweiler, "Arial Narrow Bold", sans-serif';
const THEME_BODY_FONT =
  THEME_STYLES.getPropertyValue("--font-body").trim() || 'Inter, ui-sans-serif, system-ui, sans-serif';

const plotConfig = {
  displaylogo: false,
  responsive: true,
  modeBarButtonsToRemove: ["lasso2d", "select2d"],
};

const futureScenarios = ["LIAB-01", "CLAIM-07", "TAX-14", "MED-22", "LEGAL-31"];
const memoryBudgets = [2, 4, 8, 16];
const strategies = {
  dpm: {
    label: "DPM projection",
    color: COPY.good,
    latency: [410, 520, 690, 940],
    quality: [82, 90, 94, 96],
    symbol: "diamond",
  },
  summary: {
    label: "Rolling summary",
    color: COPY.warn,
    latency: [330, 460, 740, 1280],
    quality: [58, 66, 74, 79],
    symbol: "circle",
  },
  longContext: {
    label: "Full replay",
    color: COPY.accent,
    latency: [720, 1040, 1620, 2550],
    quality: [86, 91, 95, 97],
    symbol: "square",
  },
  checkpoint: {
    label: "DPM + checkpoints",
    color: COPY.bad,
    latency: [260, 330, 470, 630],
    quality: [80, 88, 93, 95],
    symbol: "triangle-up",
  },
};

const layoutBase = {
  paper_bgcolor: "rgba(0,0,0,0)",
  plot_bgcolor: "rgba(0,0,0,0)",
  font: { color: THEME_INK, family: THEME_BODY_FONT },
  margin: { t: 38, r: 24, b: 78, l: 64 },
  xaxis: {
    gridcolor: THEME_LINE,
    zerolinecolor: THEME_LINE,
    linecolor: THEME_INK,
    tickfont: { color: THEME_MUTED, family: THEME_BODY_FONT },
    titlefont: { color: THEME_INK, family: THEME_DISPLAY_FONT, size: 15 },
  },
  yaxis: {
    gridcolor: THEME_LINE,
    zerolinecolor: THEME_LINE,
    linecolor: THEME_INK,
    tickfont: { color: THEME_MUTED, family: THEME_BODY_FONT },
    titlefont: { color: THEME_INK, family: THEME_DISPLAY_FONT, size: 15 },
  },
  hoverlabel: { bgcolor: THEME_PANEL, bordercolor: COPY.accent, font: { color: THEME_INK, family: THEME_BODY_FONT } },
  legend: { font: { color: THEME_INK, family: THEME_BODY_FONT } },
};

function chartFrame(title, overrides = {}) {
  return {
    ...layoutBase,
    title: { text: title, font: { color: THEME_INK, family: THEME_DISPLAY_FONT, size: 18 }, x: 0.02 },
    ...overrides,
  };
}

const number = new Intl.NumberFormat("en", { maximumFractionDigits: 0 });

function oneOf(...selectors) {
  for (const selector of selectors) {
    const node = document.querySelector(selector);
    if (node) {
      return node;
    }
  }
  return null;
}

function setText(selector, value) {
  const node = document.querySelector(selector);
  if (node) {
    node.textContent = value;
  }
}

function renderThemeChrome(data) {
  setText("#theme-eyebrow", COPY.eyebrow);
  setText("#theme-title", COPY.title);
  setText("#theme-deck", COPY.deck);
  setText("#theme-badge", COPY.badge);
  setText("#status-chip", data.baseline_runs.length ? "Measured baselines present" : "Baseline capture pending");
  setText("#benchmark-status", data.benchmark.current_status);
  setText("#scenario-count", number.format(data.scenario.length));
}

function renderKpis(data) {
  const target = oneOf("#hero-kpis", "#kpi-grid", "[data-kpi-grid]");
  if (!target) {
    return;
  }
  const kpis = [
    ["Scenario stages", data.scenario.length, "red-team script"],
    ["Synthetic log", "27k chars", "case evidence"],
    ["Memory budget", "5,352 chars", "projection pressure"],
    ["Baselines", data.baseline_runs.length || "pending", "measurement gate"],
  ];
  target.innerHTML = kpis
    .map(
      ([label, value, note]) => `
        <article class="kpi">
          <strong>${value}</strong>
          <span>${label}</span>
          <em>${note}</em>
        </article>
      `,
    )
    .join("");
}

function renderScenario(scenario) {
  const target = oneOf("#scenario-timeline", "#scenario-list", "[data-scenario]");
  if (!target) {
    return;
  }
  target.innerHTML = scenario
    .map(
      (item, index) => `
        <article class="stage-card">
          <span class="stage-index">${String(index + 1).padStart(2, "0")}</span>
          <div>
            <h3>${item.stage}</h3>
            <p>${item.description}</p>
            <small>${item.evidence}</small>
          </div>
        </article>
      `,
    )
    .join("");
}

function renderReadiness(readiness) {
  Plotly.newPlot(
    "readiness-chart",
    [
      {
        type: "bar",
        orientation: "h",
        y: readiness.map((item) => item.area).reverse(),
        x: readiness.map((item) => item.score).reverse(),
        customdata: readiness.map((item) => [item.status, item.recommendation]).reverse(),
        marker: {
          color: readiness
            .map((item) => (item.score >= 3 ? COPY.good : item.score === 2 ? COPY.warn : COPY.bad))
            .reverse(),
          line: { color: "rgba(255,255,255,0.38)", width: 1 },
        },
        hovertemplate:
          "<b>%{y}</b><br>Readiness: %{x}/5<br>Status: %{customdata[0]}<br><br>%{customdata[1]}<extra></extra>",
      },
      {
        type: "scatter",
        mode: "markers",
        y: readiness.map((item) => item.area).reverse(),
        x: readiness.map((item) => item.target).reverse(),
        marker: { color: COPY.accent, symbol: "line-ns-open", size: 14, line: { width: 3 } },
        hovertemplate: "Target: %{x}/5<extra></extra>",
      },
    ],
    {
      ...layoutBase,
      barmode: "overlay",
      xaxis: { ...layoutBase.xaxis, range: [0, 5], title: "Evidence readiness score" },
      yaxis: { ...layoutBase.yaxis, automargin: true },
      annotations: [{ x: 4.72, y: readiness.length - 0.5, text: "target", showarrow: false, font: { color: COPY.accent } }],
      title: { text: "READINESS VECTOR", font: { color: THEME_INK, family: THEME_DISPLAY_FONT, size: 18 }, x: 0.02 },
    },
    plotConfig,
  );
}

function renderMetricStack(metrics) {
  const quality = metrics.filter((metric) => !["Prefill wall time", "Prefill throughput"].includes(metric.metric));
  Plotly.newPlot(
    "metric-chart",
    [
      {
        type: "scatterpolar",
        r: quality.map(() => 4.2),
        theta: quality.map((metric) => metric.metric),
        fill: "toself",
        name: "Correctness evidence",
        marker: { color: COPY.good },
        line: { color: COPY.good, width: 3 },
        hovertemplate: "<b>%{theta}</b><br>Priority: high<extra></extra>",
      },
      {
        type: "scatterpolar",
        r: [3.2, 3.2],
        theta: ["Prefill wall time", "Prefill throughput"],
        fill: "toself",
        name: "Runner evidence",
        marker: { color: COPY.accent },
        line: { color: COPY.accent, width: 3 },
        hovertemplate: "<b>%{theta}</b><br>Runner source exists<extra></extra>",
      },
    ],
    {
      ...layoutBase,
      margin: { t: 44, r: 58, b: 48, l: 58 },
      polar: {
        bgcolor: "rgba(0,0,0,0)",
        radialaxis: { visible: false, range: [0, 5], gridcolor: THEME_LINE },
        angularaxis: { gridcolor: THEME_LINE, tickfont: { color: THEME_INK, family: THEME_BODY_FONT } },
      },
      showlegend: true,
      legend: { orientation: "h", y: -0.18, font: { color: THEME_INK, family: THEME_BODY_FONT } },
      title: { text: "QUALITY LOCK", font: { color: THEME_INK, family: THEME_DISPLAY_FONT, size: 18 }, x: 0.02 },
    },
    plotConfig,
  );
}

function renderReview(items) {
  const target = oneOf("#design-review-cards", "#review-grid", "[data-design-review]");
  if (!target) {
    return;
  }
  target.innerHTML = items
    .map(
      (item) => `
        <article class="review-card">
          <span>${item.score}/5</span>
          <h3>${item.area}</h3>
          <p><b>Review:</b> ${item.review}</p>
          <p><b>Upgrade:</b> ${item.upgrade}</p>
        </article>
      `,
    )
    .join("");
}

function renderMetrics(metrics) {
  const target = oneOf("#metric-cards", "[data-metrics]");
  if (!target) {
    return;
  }
  target.innerHTML = metrics
    .map(
      (metric) => `
        <article class="metric-card">
          <h3>${metric.metric}</h3>
          <strong>${metric.unit}</strong>
          <p>${metric.why_it_matters}</p>
          <small>${metric.source}</small>
        </article>
      `,
    )
    .join("");
}

function renderAssessment(rows) {
  const target = oneOf("#assessment-table", "[data-assessment-table]");
  if (!target) {
    return;
  }
  target.innerHTML = rows
    .map(
      (row) => `
        <tr>
          <td><b>${row.theme}</b></td>
          <td>${row.today}</td>
          <td>${row.improve}</td>
          <td>${row.data_needed}</td>
        </tr>
      `,
    )
    .join("");
}

function summarizeRun(run) {
  const wall = run.iterations.map((iteration) => iteration.wall_ms).sort((a, b) => a - b);
  const p50 = wall[Math.floor((wall.length - 1) * 0.5)];
  const p95 = wall[Math.floor((wall.length - 1) * 0.95)];
  return { label: `${run.backend} / ${run.model_path_basename || "model"}`, p50, p95 };
}

function renderBaselines(runs) {
  const empty = oneOf("#baseline-empty", "[data-baseline-empty]");
  const panel = oneOf("#baseline-panel", "[data-baseline-panel]");
  const chart = oneOf("#baseline-chart");
  if (!runs.length) {
    if (empty) {
      empty.hidden = false;
    }
    return;
  }
  if (empty) {
    empty.hidden = true;
  }
  if (panel) {
    panel.hidden = false;
  }
  if (!chart) {
    return;
  }
  chart.hidden = false;
  const summaries = runs.map(summarizeRun);
  Plotly.newPlot(
    "baseline-chart",
    [
      { type: "bar", name: "p50 wall time", x: summaries.map((item) => item.label), y: summaries.map((item) => item.p50), marker: { color: COPY.accent } },
      { type: "bar", name: "p95 wall time", x: summaries.map((item) => item.label), y: summaries.map((item) => item.p95), marker: { color: COPY.warn } },
    ],
    {
      ...layoutBase,
      barmode: "group",
      yaxis: { ...layoutBase.yaxis, title: "Wall time (ms)" },
      title: { text: "BASELINE TELEMETRY", font: { color: THEME_INK, family: THEME_DISPLAY_FONT, size: 18 }, x: 0.02 },
    },
    plotConfig,
  );
}

function renderFutureFrontier() {
  if (!document.querySelector("#frontier-chart")) return;
  Plotly.newPlot(
    "frontier-chart",
    Object.values(strategies).map((strategy) => ({
      type: "scatter",
      mode: "lines+markers",
      name: strategy.label,
      x: strategy.latency,
      y: strategy.quality,
      marker: { color: strategy.color, size: 12, symbol: strategy.symbol, line: { color: THEME_INK, width: 2 } },
      line: { color: strategy.color, width: 4 },
      hovertemplate: "<b>%{fullData.name}</b><br>Latency: %{x} ms<br>Audit quality: %{y}%<extra></extra>",
    })),
    chartFrame("QUALITY / SPEED FRONTIER", {
      xaxis: { ...layoutBase.xaxis, title: "Replay latency placeholder (ms)" },
      yaxis: { ...layoutBase.yaxis, title: "Audit quality placeholder (%)", range: [50, 100] },
      shapes: [{ type: "line", x0: 650, x1: 650, y0: 50, y1: 100, line: { color: COPY.accent, width: 2, dash: "dot" } }],
      annotations: [{ x: 650, y: 98, text: "target latency lane", showarrow: false, font: { color: COPY.accent, family: THEME_DISPLAY_FONT } }],
    }),
    plotConfig,
  );
}

function renderFutureBudgetCurves() {
  if (!document.querySelector("#budget-chart")) return;
  Plotly.newPlot(
    "budget-chart",
    Object.values(strategies).map((strategy) => ({
      type: "scatter",
      mode: "lines+markers",
      name: strategy.label,
      x: memoryBudgets,
      y: strategy.quality,
      marker: { color: strategy.color, size: 10 },
      line: { color: strategy.color, width: 3 },
      hovertemplate: "<b>%{fullData.name}</b><br>Budget: %{x}k chars<br>Fact/citation score: %{y}%<extra></extra>",
    })),
    chartFrame("MEMORY BUDGET CURVES", {
      xaxis: { ...layoutBase.xaxis, title: "Projection budget placeholder (k chars)" },
      yaxis: { ...layoutBase.yaxis, title: "Quality placeholder (%)", range: [50, 100] },
    }),
    plotConfig,
  );
}

function renderFuturePassMatrix() {
  if (!document.querySelector("#passfail-chart")) return;
  const gates = ["Fact retention", "Citation validity", "Correction", "Determinism", "Latency SLA"];
  const values = [
    [1, 1, 0.5, 1, 0.5],
    [1, 0.5, 1, 1, 1],
    [0.5, 1, 0.5, 1, 0],
    [1, 1, 1, 0.5, 0.5],
    [0.5, 0.5, 1, 1, 1],
  ];
  Plotly.newPlot(
    "passfail-chart",
    [{
      type: "heatmap",
      x: gates,
      y: futureScenarios,
      z: values,
      colorscale: [[0, COPY.bad], [0.5, COPY.warn], [1, COPY.good]],
      showscale: false,
      hovertemplate: "<b>%{y}</b><br>%{x}: %{z}<extra></extra>",
    }],
    chartFrame("RED-TEAM PASS MATRIX", { xaxis: { ...layoutBase.xaxis, automargin: true }, yaxis: { ...layoutBase.yaxis, automargin: true } }),
    plotConfig,
  );
}

function renderFutureEvidenceFlow() {
  if (!document.querySelector("#evidence-flow-chart")) return;
  Plotly.newPlot(
    "evidence-flow-chart",
    [{
      type: "sankey",
      arrangement: "fixed",
      node: {
        pad: 18,
        thickness: 18,
        line: { color: THEME_INK, width: 2 },
        label: ["Planted facts", "Corrections", "Distractors", "Projection", "Cited answer", "Dropped facts"],
        color: [COPY.accent, COPY.warn, THEME_MUTED, COPY.good, COPY.good, COPY.bad],
      },
      link: {
        source: [0, 1, 2, 3, 3],
        target: [3, 3, 3, 4, 5],
        value: [42, 9, 28, 46, 5],
        color: ["rgba(245,255,0,.45)", "rgba(255,138,0,.45)", "rgba(52,52,63,.25)", "rgba(0,240,255,.45)", "rgba(255,43,214,.45)"],
      },
    }],
    chartFrame("EVIDENCE COVERAGE MAP", { font: { color: THEME_INK, family: THEME_BODY_FONT }, margin: { t: 48, r: 20, b: 30, l: 20 } }),
    plotConfig,
  );
}

function renderFutureDeterminism() {
  if (!document.querySelector("#determinism-chart")) return;
  const runs = ["R1", "R2", "R3", "R4", "R5", "R6"];
  Plotly.newPlot(
    "determinism-chart",
    [{
      type: "heatmap",
      x: runs,
      y: futureScenarios,
      z: futureScenarios.map((_, row) => runs.map((__, col) => (row + col) % 7 === 0 ? 0 : 1)),
      colorscale: [[0, COPY.bad], [1, COPY.good]],
      showscale: false,
      hovertemplate: "<b>%{y}</b><br>%{x}: hash %{z}<extra></extra>",
    }],
    chartFrame("RUN-TO-RUN DETERMINISM", { xaxis: { ...layoutBase.xaxis, title: "Seeded replay run" }, yaxis: { ...layoutBase.yaxis, automargin: true } }),
    plotConfig,
  );
}

function renderFutureCorrectionRecovery() {
  if (!document.querySelector("#correction-chart")) return;
  Plotly.newPlot(
    "correction-chart",
    [
      { type: "scatter", mode: "lines+markers", name: "DPM projection", x: [0, 1, 2, 3, 4], y: [35, 41, 91, 95, 96], line: { color: COPY.good, width: 4 } },
      { type: "scatter", mode: "lines+markers", name: "Rolling summary", x: [0, 1, 2, 3, 4], y: [34, 38, 54, 62, 68], line: { color: COPY.warn, width: 4 } },
    ],
    chartFrame("CORRECTION RECOVERY TIMELINE", {
      xaxis: { ...layoutBase.xaxis, title: "Turns after correction event" },
      yaxis: { ...layoutBase.yaxis, title: "Recovered state score (%)", range: [20, 100] },
      shapes: [{ type: "line", x0: 2, x1: 2, y0: 20, y1: 100, line: { color: COPY.accent, dash: "dash", width: 2 } }],
    }),
    plotConfig,
  );
}

function renderFutureErrorTaxonomy() {
  if (!document.querySelector("#error-taxonomy-chart")) return;
  Plotly.newPlot(
    "error-taxonomy-chart",
    [{
      type: "bar",
      orientation: "h",
      x: [3, 4, 2, 1, 5, 2],
      y: ["Dropped fact", "Invalid citation", "Contradiction", "Hallucinated anchor", "Late correction", "Output variance"],
      marker: { color: [COPY.good, COPY.warn, COPY.good, COPY.good, COPY.bad, COPY.warn], line: { color: THEME_INK, width: 2 } },
      hovertemplate: "%{y}: %{x} placeholder failures<extra></extra>",
    }],
    chartFrame("ERROR TAXONOMY", { xaxis: { ...layoutBase.xaxis, title: "Placeholder failure count" }, yaxis: { ...layoutBase.yaxis, automargin: true } }),
    plotConfig,
  );
}

function renderFutureRawRunExplorer() {
  if (!document.querySelector("#raw-run-chart")) return;
  Plotly.newPlot(
    "raw-run-chart",
    [{
      type: "table",
      header: { values: ["run_id", "strategy", "model", "backend", "commit", "score"], fill: { color: THEME_INK }, font: { color: "#f5f1e8", family: THEME_DISPLAY_FONT, size: 14 } },
      cells: {
        values: [
          ["rt-001", "rt-002", "rt-003", "rt-004"],
          ["DPM", "Summary", "DPM+checkpoint", "Full replay"],
          ["Gemma", "Gemma", "Gemma", "Gemma"],
          ["CPU", "CPU", "NPU", "GPU"],
          ["abc123", "abc123", "abc123", "abc123"],
          ["94", "68", "93", "96"],
        ],
        fill: { color: "rgba(255,255,255,.72)" },
        font: { color: THEME_INK, family: THEME_BODY_FONT, size: 13 },
      },
    }],
    chartFrame("RAW RUN EXPLORER", { margin: { t: 48, r: 12, b: 12, l: 12 } }),
    plotConfig,
  );
}

function renderFutureSuitePlaceholders() {
  renderFutureFrontier();
  renderFutureBudgetCurves();
  renderFuturePassMatrix();
  renderFutureEvidenceFlow();
  renderFutureDeterminism();
  renderFutureCorrectionRecovery();
  renderFutureErrorTaxonomy();
  renderFutureRawRunExplorer();
}

fetch("./dpm-red-team-benchmark-data.json")
  .then((response) => response.json())
  .then((data) => {
    renderThemeChrome(data);
    renderKpis(data);
    renderScenario(data.scenario);
    renderReadiness(data.readiness);
    renderMetricStack(data.recommended_metrics);
    renderReview(data.design_review);
    renderMetrics(data.recommended_metrics);
    renderAssessment(data.chart_assessment);
    renderBaselines(data.baseline_runs);
    renderFutureSuitePlaceholders();
  })
  .catch((error) => {
    setText("#benchmark-status", `Could not load page data: ${error.message}`);
  });
