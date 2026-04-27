(function () {
  const React = window.React;
  const ReactDOM = window.ReactDOM;

  if (!React || !ReactDOM) {
    return;
  }

  const h = React.createElement;
  const { useMemo, useState } = React;

  const scenarioTracks = [
    {
      id: "LIAB-01",
      title: "Warehouse liability dispute",
      risk: "policy limit + invoice correction",
      events: [
        {
          label: "E01",
          type: "fact planted",
          text: "Policy limit is $100,000; deductible is $500.",
          citation: "[4]",
          status: "retained",
        },
        {
          label: "E07",
          type: "distractor",
          text: "Outdated invoice total appears in a tool response.",
          citation: "[11]",
          status: "discarded",
        },
        {
          label: "E12",
          type: "correction",
          text: "Correction event replaces invoice INV-7022 with INV-7041.",
          citation: "[18]",
          status: "repaired",
        },
      ],
      projection:
        "Facts retain policy limit, deductible, corrected invoice id, and Regulation B anchor with one-based citations.",
      answer:
        "Proceed with liability analysis using INV-7041; cite policy limit and correction event before any settlement recommendation.",
      scores: { retention: 96, citation: 94, recovery: 91, determinism: 100 },
    },
    {
      id: "TAX-14",
      title: "Adversarial tax deduction review",
      risk: "contradictory tool evidence",
      events: [
        {
          label: "E03",
          type: "fact planted",
          text: "Vehicle deduction depends on logged business mileage.",
          citation: "[5]",
          status: "retained",
        },
        {
          label: "E09",
          type: "contradiction",
          text: "Later receipt claims personal-use miles were included.",
          citation: "[13]",
          status: "flagged",
        },
        {
          label: "E16",
          type: "audit anchor",
          text: "Compliance anchor requires explicit substantiation warning.",
          citation: "[21]",
          status: "retained",
        },
      ],
      projection:
        "Projection preserves mileage facts, contradiction marker, and substantiation warning instead of smoothing the conflict.",
      answer:
        "Do not approve deduction until personal-use miles are separated and substantiation is attached.",
      scores: { retention: 92, citation: 89, recovery: 84, determinism: 98 },
    },
    {
      id: "MED-22",
      title: "Clinical prior-auth memory pressure",
      risk: "critical negation under compression",
      events: [
        {
          label: "E02",
          type: "fact planted",
          text: "Patient has not tried medication A.",
          citation: "[3]",
          status: "retained",
        },
        {
          label: "E10",
          type: "distractor",
          text: "Similar patient record says medication A failed.",
          citation: "[15]",
          status: "discarded",
        },
        {
          label: "E19",
          type: "audit anchor",
          text: "Decision must cite trial-history requirements.",
          citation: "[26]",
          status: "retained",
        },
      ],
      projection:
        "Projection keeps the negation and separates the similar-record distractor from the active patient.",
      answer:
        "Prior authorization remains incomplete because required medication trial history is absent.",
      scores: { retention: 90, citation: 88, recovery: 86, determinism: 99 },
    },
  ];

  const evidenceRows = [
    {
      fact: "Policy limit",
      raw: "[4] $100,000 policy cap",
      projection: "Facts[0] cap retained",
      answer: "cited in liability recommendation",
      score: 100,
      status: "covered",
    },
    {
      fact: "Corrected invoice",
      raw: "[18] INV-7041 replaces INV-7022",
      projection: "Facts[2] correction applied",
      answer: "uses corrected invoice only",
      score: 96,
      status: "covered",
    },
    {
      fact: "Contradiction",
      raw: "[13] personal-use miles included",
      projection: "Reasoning[1] conflict flagged",
      answer: "blocks approval",
      score: 91,
      status: "covered",
    },
    {
      fact: "Compliance anchor",
      raw: "[21] substantiation warning",
      projection: "Compliance[0] retained",
      answer: "cited before decision",
      score: 88,
      status: "watch",
    },
    {
      fact: "Distractor record",
      raw: "[15] similar patient failed med A",
      projection: "excluded as non-active patient",
      answer: "not cited",
      score: 84,
      status: "watch",
    },
  ];

  function ScorePill({ label, value }) {
    return h(
      "div",
      { className: "react-score-pill" },
      h("span", null, label),
      h("strong", null, `${value}%`),
    );
  }

  function ScenarioReplayExplorer() {
    const [activeTrack, setActiveTrack] = useState(0);
    const [activeEvent, setActiveEvent] = useState(0);
    const track = scenarioTracks[activeTrack];
    const event = track.events[activeEvent];

    return h(
      "article",
      { className: "react-demo-card scenario-replay" },
      h(
        "div",
        { className: "react-demo-head" },
        h("div", null, h("p", { className: "eyebrow" }, "React component 01"), h("h2", null, "Scenario replay explorer")),
        h("span", { className: "tag" }, "interactive placeholder"),
      ),
      h(
        "div",
        { className: "react-track-tabs", role: "tablist", "aria-label": "Scenario tracks" },
        scenarioTracks.map((item, index) =>
          h(
            "button",
            {
              key: item.id,
              className: index === activeTrack ? "is-active" : "",
              onClick: () => {
                setActiveTrack(index);
                setActiveEvent(0);
              },
            },
            item.id,
          ),
        ),
      ),
      h(
        "div",
        { className: "replay-grid" },
        h(
          "section",
          { className: "replay-lane" },
          h("h3", null, track.title),
          h("p", { className: "microcopy" }, track.risk),
          h(
            "div",
            { className: "event-stack" },
            track.events.map((item, index) =>
              h(
                "button",
                {
                  key: item.label,
                  className: index === activeEvent ? "event-card is-active" : "event-card",
                  onClick: () => setActiveEvent(index),
                },
                h("span", null, item.label),
                h("strong", null, item.type),
                h("small", null, item.citation),
              ),
            ),
          ),
        ),
        h(
          "section",
          { className: "replay-detail" },
          h("p", { className: "eyebrow" }, `${event.status} / ${event.citation}`),
          h("h3", null, event.text),
          h("div", { className: "telemetry-divider" }),
          h("p", null, h("strong", null, "Projection: "), track.projection),
          h("p", null, h("strong", null, "Answer: "), track.answer),
          h(
            "div",
            { className: "score-grid" },
            Object.entries(track.scores).map(([label, value]) => h(ScorePill, { key: label, label, value })),
          ),
        ),
      ),
    );
  }

  function EvidenceCoverageVisualization() {
    const [selected, setSelected] = useState(evidenceRows[0].fact);
    const selectedRow = useMemo(
      () => evidenceRows.find((row) => row.fact === selected) || evidenceRows[0],
      [selected],
    );

    return h(
      "article",
      { className: "react-demo-card evidence-coverage" },
      h(
        "div",
        { className: "react-demo-head" },
        h("div", null, h("p", { className: "eyebrow" }, "React component 02"), h("h2", null, "Evidence coverage visualization")),
        h("span", { className: "tag" }, "coverage map"),
      ),
      h(
        "div",
        { className: "coverage-map" },
        evidenceRows.map((row) =>
          h(
            "button",
            {
              key: row.fact,
              className: row.fact === selected ? `coverage-row ${row.status} is-active` : `coverage-row ${row.status}`,
              onClick: () => setSelected(row.fact),
            },
            h("span", { className: "coverage-node raw" }, row.fact),
            h("span", { className: "coverage-link" }),
            h("span", { className: "coverage-node projection" }, `${row.score}%`),
            h("span", { className: "coverage-link" }),
            h("span", { className: "coverage-node answer" }, row.status),
          ),
        ),
      ),
      h(
        "div",
        { className: "coverage-detail" },
        h("h3", null, selectedRow.fact),
        h(
          "dl",
          null,
          h("dt", null, "Raw event"),
          h("dd", null, selectedRow.raw),
          h("dt", null, "Projection"),
          h("dd", null, selectedRow.projection),
          h("dt", null, "Answer usage"),
          h("dd", null, selectedRow.answer),
        ),
        h(
          "div",
          { className: "coverage-meter", "aria-label": `Coverage ${selectedRow.score}%` },
          h("span", { style: { width: `${selectedRow.score}%` } }),
        ),
      ),
    );
  }

  function mount(Component, id) {
    const node = document.getElementById(id);
    if (node) {
      ReactDOM.createRoot(node).render(h(Component));
    }
  }

  mount(ScenarioReplayExplorer, "scenario-replay-root");
  mount(EvidenceCoverageVisualization, "evidence-coverage-root");
})();
