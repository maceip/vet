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

const plotConfig = {
  displaylogo: false,
  responsive: true,
  modeBarButtonsToRemove: ["lasso2d", "select2d"],
};

const layoutBase = {
  paper_bgcolor: "rgba(0,0,0,0)",
  plot_bgcolor: "rgba(0,0,0,0)",
  font: { color: "var(--ink)", family: "Inter, ui-sans-serif, system-ui" },
  margin: { t: 30, r: 18, b: 72, l: 58 },
  xaxis: { gridcolor: "rgba(80, 80, 80, 0.22)", zerolinecolor: "rgba(80, 80, 80, 0.28)" },
  yaxis: { gridcolor: "rgba(80, 80, 80, 0.22)", zerolinecolor: "rgba(80, 80, 80, 0.28)" },
  hoverlabel: { bgcolor: "rgba(20, 20, 30, 0.94)", bordercolor: COPY.accent },
};

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
        radialaxis: { visible: false, range: [0, 5], gridcolor: "rgba(80,80,80,0.22)" },
        angularaxis: { gridcolor: "rgba(80,80,80,0.22)" },
      },
      showlegend: true,
      legend: { orientation: "h", y: -0.18 },
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
    { ...layoutBase, barmode: "group", yaxis: { ...layoutBase.yaxis, title: "Wall time (ms)" } },
    plotConfig,
  );
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
  })
  .catch((error) => {
    setText("#benchmark-status", `Could not load page data: ${error.message}`);
  });
