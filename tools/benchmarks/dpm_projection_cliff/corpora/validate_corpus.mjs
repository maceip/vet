// Validates the cliff bench corpus by mirroring the parsing + event
// classification rules in cliff_corpus.cc + cliff_handoff.cc. Runs over
// every *.yaml in a directory and prints per-case load/classify
// statistics plus a summary, so we can prove the corpus is integration-
// ready without waiting on a C++ build. Exit 0 iff every case loads and
// every case has a non-empty trajectory + at least one routed probe.
//
// Usage:
//   node validate_corpus.mjs <corpus_dir>
//
// Mirrors the C++ parser semantics:
//   - top-level scalars (case_id, domain, seed, decision_label, citation)
//   - trajectory: list of {- type, timestamp, payload (block scalar)}
//   - ground_truth: scalars + reasoning_anchors list
//   - probes: list of {axis, question, expected_exact|expected_substrings,
//     judge_rubric}
// And the event classifier:
//   - explicit handoff types and HANDOFF/handoff_to_tier/escalate_to in payload
//   - correction events
//   - milestone via 13 MITRE technique IDs
//   - user / model / tool buckets
// And the simulated-policy walker:
//   - min_delta_tokens=256, max_delta_tokens=1024
//   - checkpoint_on_handoff/correction/milestone_tool=true

import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, basename } from "node:path";

const MILESTONE_TECHNIQUES = [
  "T1059","T1003","T1021","T1078","T1486","T1190","T1133",
  "T1547","T1098","T1071","T1041","T1083","T1135",
];
const HANDOFF_TYPES = new Set([
  "handoff_request","tier_escalation","shift_change","specialist_consult",
]);

function classifyEvent(yamlType, yamlPayload) {
  if (HANDOFF_TYPES.has(yamlType)) return "handoff";
  if (yamlPayload.includes("HANDOFF") ||
      yamlPayload.includes("handoff_to_tier") ||
      yamlPayload.includes("escalate_to")) return "handoff";
  if (yamlType === "correction" || yamlType === "analyst_correction" ||
      yamlPayload.includes("CORRECTION")) return "correction";
  for (const t of MILESTONE_TECHNIQUES) {
    if (yamlPayload.includes(t)) return "milestone";
  }
  if (yamlType === "user_message" || yamlType === "customer_message" ||
      yamlType === "ticket_create") return "user";
  if (yamlType === "agent_response" || yamlType === "model_response" ||
      yamlType === "model") return "model";
  return "tool";
}

function indentOf(line) {
  let n = 0; while (n < line.length && line[n] === " ") n++; return n;
}
function isBlankOrComment(line) {
  const s = line.trim(); return s.length === 0 || s[0] === "#";
}
function stripQuotes(s) {
  s = s.trim();
  if (s.length >= 2 &&
      ((s[0] === '"' && s[s.length-1] === '"') ||
       (s[0] === "'" && s[s.length-1] === "'"))) {
    return s.slice(1, -1);
  }
  return s;
}
function parseFlowList(body) {
  body = body.trim();
  if (body.length < 2 || body[0] !== "[" || body[body.length-1] !== "]") return [];
  const out = []; let cur = ""; let q = false; let qc = "";
  for (const c of body.slice(1, -1)) {
    if (q) { if (c === qc) q = false; else cur += c; continue; }
    if (c === '"' || c === "'") { q = true; qc = c; continue; }
    if (c === ",") { out.push(cur.trim()); cur = ""; continue; }
    cur += c;
  }
  if (cur.length) out.push(cur.trim());
  return out;
}
function readBlockScalar(lines, idx, baseIndent) {
  let value = ""; let i = idx + 1; let contentIndent = -1;
  while (i < lines.length) {
    const l = lines[i];
    if (isBlankOrComment(l)) { value += "\n"; i++; continue; }
    const ind = indentOf(l);
    if (ind <= baseIndent) break;
    if (contentIndent < 0) contentIndent = ind;
    value += l.slice(Math.min(contentIndent, l.length)) + "\n";
    i++;
  }
  return { value, nextIdx: i };
}

function parseCase(text) {
  const lines = text.replace(/\r/g, "").split("\n");
  const out = {
    case_id: "", domain: "", seed: 0,
    events: [], event_log: "",
    decision_label: "", citation: "",
    reasoning_anchors: [],
    probes: [],
    rcs_rubric: "", crr_rubric: "",
  };
  let section = "top";
  let cur = null;
  let probe = null;
  let eventIndex = 1;

  const flushEvent = () => {
    if (!cur) return;
    const bucket = classifyEvent(cur.type, cur.payload);
    const rendered = `[${eventIndex++}] {"type":"${cur.type}",`+
      `"timestamp":"${cur.timestamp}","payload":${JSON.stringify(cur.payload)}}\n`;
    out.event_log += rendered;
    out.events.push({
      text: rendered, approx_tokens: Math.floor(rendered.length / 4),
      type_bucket: bucket, raw_type: cur.type, raw_payload: cur.payload,
    });
    cur = null;
  };
  const flushProbe = () => {
    if (!probe) return;
    if (probe.axis === "rcs") out.rcs_rubric = probe.judge_rubric || "";
    if (probe.axis === "crr") out.crr_rubric = probe.judge_rubric || "";
    out.probes.push(probe);
    probe = null;
  };

  for (let i = 0; i < lines.length; ) {
    const line = lines[i];
    if (isBlankOrComment(line)) { i++; continue; }
    const indent = indentOf(line);
    const body = line.trim();
    if (indent === 0) {
      flushEvent(); flushProbe();
      const colon = body.indexOf(":");
      if (colon < 0) { i++; continue; }
      const key = body.slice(0, colon).trim();
      const rhs = body.slice(colon+1).trim();
      if (key === "case_id") out.case_id = stripQuotes(rhs);
      else if (key === "domain") out.domain = stripQuotes(rhs);
      else if (key === "seed") out.seed = parseInt(rhs, 10) || 0;
      else if (key === "trajectory") section = "trajectory";
      else if (key === "ground_truth") section = "ground_truth";
      else if (key === "probes") section = "probes";
      else section = "top";
      i++; continue;
    }
    if (section === "trajectory") {
      if (body.startsWith("- ")) {
        flushEvent();
        cur = { type: "", timestamp: "", payload: "" };
        const rest = body.slice(2);
        const colon = rest.indexOf(":");
        if (colon >= 0) {
          const k = rest.slice(0, colon).trim();
          const v = rest.slice(colon+1).trim();
          if (k === "type") cur.type = stripQuotes(v);
          else if (k === "timestamp") cur.timestamp = stripQuotes(v);
          else if (k === "payload") {
            if (v === "|") {
              const blk = readBlockScalar(lines, i, indent);
              cur.payload = blk.value; i = blk.nextIdx; continue;
            } else cur.payload = stripQuotes(v);
          }
        }
        i++; continue;
      }
      const colon = body.indexOf(":");
      if (colon < 0) { i++; continue; }
      const k = body.slice(0, colon).trim();
      const v = body.slice(colon+1).trim();
      if (cur) {
        if (k === "type") cur.type = stripQuotes(v);
        else if (k === "timestamp") cur.timestamp = stripQuotes(v);
        else if (k === "payload") {
          if (v === "|") {
            const blk = readBlockScalar(lines, i, indent);
            cur.payload = blk.value; i = blk.nextIdx; continue;
          } else cur.payload = stripQuotes(v);
        }
      }
      i++; continue;
    }
    if (section === "ground_truth") {
      const colon = body.indexOf(":");
      if (body.startsWith("- ")) {
        out.reasoning_anchors.push(stripQuotes(body.slice(2)));
      } else if (colon >= 0) {
        const k = body.slice(0, colon).trim();
        const v = body.slice(colon+1).trim();
        if (k === "decision_label") out.decision_label = stripQuotes(v);
        else if (k === "citation") out.citation = stripQuotes(v);
        else if (k === "reasoning_anchors" && v.startsWith("[")) {
          for (const s of parseFlowList(v)) out.reasoning_anchors.push(stripQuotes(s));
        }
      }
      i++; continue;
    }
    if (section === "probes") {
      if (body.startsWith("- ")) {
        flushProbe();
        probe = { axis: "", question: "", expected_exact: "", expected_substrings: [], judge_rubric: "" };
        const rest = body.slice(2);
        const colon = rest.indexOf(":");
        if (colon >= 0) {
          const k = rest.slice(0, colon).trim();
          const v = rest.slice(colon+1).trim();
          if (k === "axis") probe.axis = stripQuotes(v);
        }
        i++; continue;
      }
      const colon = body.indexOf(":");
      if (colon < 0) {
        if (body.startsWith("- ") && probe) {
          probe.expected_substrings.push(stripQuotes(body.slice(2)));
        }
        i++; continue;
      }
      const k = body.slice(0, colon).trim();
      const v = body.slice(colon+1).trim();
      if (probe) {
        if (k === "axis") probe.axis = stripQuotes(v);
        else if (k === "question") {
          if (v === "|") {
            const blk = readBlockScalar(lines, i, indent);
            probe.question = blk.value.trim(); i = blk.nextIdx; continue;
          } else probe.question = stripQuotes(v);
        } else if (k === "expected_exact") probe.expected_exact = stripQuotes(v);
        else if (k === "expected_substrings") {
          if (v.startsWith("[")) {
            for (const s of parseFlowList(v)) probe.expected_substrings.push(stripQuotes(s));
          }
        } else if (k === "judge_rubric") {
          if (v === "|") {
            const blk = readBlockScalar(lines, i, indent);
            probe.judge_rubric = blk.value.trim(); i = blk.nextIdx; continue;
          } else probe.judge_rubric = stripQuotes(v);
        }
      }
      i++; continue;
    }
    i++;
  }
  flushEvent(); flushProbe();
  return out;
}

function simulatePolicy(events) {
  const trace = [];
  const policy = {
    min_delta_tokens: 256, max_delta_tokens: 1024,
    on_handoff: true, on_correction: true, on_milestone: true,
    context_pressure_ratio: 0.85,
  };
  const max_ctx = 32768;
  let tokens_since = 0;
  let current_ctx = 0;
  for (let i = 0; i < events.length; i++) {
    const e = events[i];
    tokens_since += e.approx_tokens;
    current_ctx += e.approx_tokens;
    let trigger = null;
    if (e.type_bucket === "handoff" && policy.on_handoff) trigger = "TRIGGER_HANDOFF";
    else if (e.type_bucket === "correction" && policy.on_correction) trigger = "TRIGGER_HANDOFF";
    else if (e.type_bucket === "milestone" && policy.on_milestone &&
             tokens_since >= policy.min_delta_tokens) trigger = "TRIGGER_MILESTONE_TOOL";
    else if (tokens_since >= policy.max_delta_tokens) trigger = "TRIGGER_TOKEN_THRESHOLD";
    else if (current_ctx / max_ctx >= policy.context_pressure_ratio &&
             tokens_since >= policy.min_delta_tokens) trigger = "TRIGGER_TOKEN_THRESHOLD";
    if (trigger) {
      trace.push({ event_index: i, trigger, tokens_since, bucket: e.type_bucket });
      tokens_since = 0;
    }
  }
  return trace;
}

function findHandoffIndex(events) {
  for (let i = 0; i < events.length; i++) {
    if (events[i].type_bucket === "handoff") return i;
  }
  return -1;
}

const SEVERE_MILESTONES = ["T1003", "T1021", "T1078", "T1486"];

function resolveHandoffIndex(events) {
  if (events.length === 0) return { index: -1, kind: "none" };
  const explicit = findHandoffIndex(events);
  if (explicit >= 0) return { index: explicit, kind: "explicit" };
  for (let i = 0; i < events.length; i++) {
    for (const t of SEVERE_MILESTONES) {
      if (events[i].text.includes(t)) {
        return { index: i, kind: "synthetic_severe_milestone" };
      }
    }
  }
  for (let i = 0; i < events.length; i++) {
    if (events[i].type_bucket === "milestone") {
      return { index: i, kind: "synthetic_milestone" };
    }
  }
  return { index: Math.floor(events.length / 2), kind: "synthetic_median" };
}

function validateCase(path) {
  const text = readFileSync(path, "utf8");
  const c = parseCase(text);
  const errs = [];
  if (!c.case_id) errs.push("missing case_id");
  if (c.events.length === 0) errs.push("empty trajectory");
  if (!c.decision_label) errs.push("missing decision_label");
  const probesRouted = c.probes.filter(p =>
    p.axis === "frp" || p.axis === "rcs" || p.axis === "eda" || p.axis === "crr").length;
  if (probesRouted === 0) errs.push("no axis-routed probes");
  const buckets = { handoff: 0, correction: 0, milestone: 0, user: 0, model: 0, tool: 0 };
  for (const e of c.events) buckets[e.type_bucket] = (buckets[e.type_bucket] || 0) + 1;
  const trace = simulatePolicy(c.events);
  const resolved = resolveHandoffIndex(c.events);
  return { path: basename(path), case_id: c.case_id, ok: errs.length === 0, errs,
           events: c.events.length, buckets, anchors: c.reasoning_anchors.length,
           probes: c.probes.length, probesRouted, decision_label: c.decision_label,
           checkpoints: trace.length, handoffIdx: resolved.index,
           handoffKind: resolved.kind,
           triggers: trace.reduce((m, t) => { m[t.trigger] = (m[t.trigger]||0)+1; return m; }, {}) };
}

function main() {
  const dir = process.argv[2];
  if (!dir) {
    console.error("usage: node validate_corpus.mjs <corpus_dir>");
    process.exit(2);
  }
  const entries = readdirSync(dir)
    .filter(n => n.endsWith(".yaml") || n.endsWith(".yml"))
    .map(n => join(dir, n))
    .filter(p => statSync(p).isFile())
    .sort();
  if (entries.length === 0) {
    console.error(`no .yaml under ${dir}`);
    process.exit(2);
  }
  const results = entries.map(validateCase);
  const okCount = results.filter(r => r.ok).length;
  const failed = results.filter(r => !r.ok);
  const totalEvents = results.reduce((s, r) => s + r.events, 0);
  const totalCheckpoints = results.reduce((s, r) => s + r.checkpoints, 0);
  const sumBuckets = results.reduce((m, r) => {
    for (const [k, v] of Object.entries(r.buckets)) m[k] = (m[k]||0) + v;
    return m;
  }, {});
  const sumTriggers = results.reduce((m, r) => {
    for (const [k, v] of Object.entries(r.triggers)) m[k] = (m[k]||0) + v;
    return m;
  }, {});
  const handoffCount = results.filter(r => r.handoffIdx >= 0).length;
  const kindHist = results.reduce((m, r) => {
    m[r.handoffKind] = (m[r.handoffKind] || 0) + 1; return m;
  }, {});

  console.log("# DPM cliff bench corpus validation");
  console.log("");
  console.log(`directory: ${dir}`);
  console.log(`cases:     ${results.length} (${okCount} OK, ${failed.length} FAIL)`);
  console.log("");
  console.log("## per-case summary (first 20 + last 5)");
  const head = results.slice(0, 20);
  const tail = results.slice(-5);
  for (const r of head) {
    console.log(`  ${r.ok ? "OK " : "FAIL"}  ${r.case_id.padEnd(36)}  evts=${String(r.events).padStart(3)}  ckpts=${String(r.checkpoints).padStart(2)}  ho_idx=${String(r.handoffIdx).padStart(3)}  ms=${r.buckets.milestone}  ho=${r.buckets.handoff}  cor=${r.buckets.correction}`);
  }
  if (results.length > 25) console.log("  ...");
  for (const r of tail) {
    console.log(`  ${r.ok ? "OK " : "FAIL"}  ${r.case_id.padEnd(36)}  evts=${String(r.events).padStart(3)}  ckpts=${String(r.checkpoints).padStart(2)}  ho_idx=${String(r.handoffIdx).padStart(3)}  ms=${r.buckets.milestone}  ho=${r.buckets.handoff}  cor=${r.buckets.correction}`);
  }
  console.log("");
  console.log("## aggregate");
  console.log(`  total events:      ${totalEvents}`);
  console.log(`  total checkpoints: ${totalCheckpoints} (avg ${(totalCheckpoints/results.length).toFixed(1)}/case)`);
  console.log(`  cases w/ handoff:  ${handoffCount}/${results.length}`);
  console.log(`  handoff_kind hist: ${JSON.stringify(kindHist)}`);
  console.log(`  bucket totals:     ${JSON.stringify(sumBuckets)}`);
  console.log(`  trigger totals:    ${JSON.stringify(sumTriggers)}`);
  if (failed.length) {
    console.log("");
    console.log("## failures");
    for (const f of failed) {
      console.log(`  ${f.path}  errs=${f.errs.join(", ")}`);
    }
  }
  process.exit(failed.length === 0 ? 0 : 1);
}

main();
