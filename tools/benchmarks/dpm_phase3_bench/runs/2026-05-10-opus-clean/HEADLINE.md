# Phase 3 bench — clean run on the validity-fixed bench

Run: `runs/2026-05-10-opus-clean/` · Branch `phase3-bench` at HEAD `7126cf6c`
(validity fixes A-F + symmetric system prompts) · Run host: EC2 `zunft-core`
(eu-central-1) · Model: `claude-opus-4-7` · Adapter: `AnthropicModelAdapter`.

**54 rows, 0 errored.** Matrix is 6 fixtures × 3 conditions × 3 repeats = 18
distinct cells × 3 repeats per cell. (No more inflated test_kind axis.)

Total API spend on this run: **$56.00** (3.15M input + 117K output tokens).

---

## Headline result — honest version

| condition | scored cells | mean | stddev | stale-escape rate |
|---|---|---|---|---|
| **raw_oracle** | 18 | **0.7176** | 0.378 | **1.000** |
| rolling_summary | 18 | 0.5602 | 0.422 | 0.667 |
| **dpm_phase3_checkpoint** | 18 | **0.4861** | 0.466 | **0.000** |

This is the bench result with the rubric leakage closed (Fix A), the
inflated `test_kind` axis dropped (Fix C), substring-based correction
detection replaced with typed directives (Fix B), chart guards wired
(Fix D), and symmetric system prompts across conditions. **All numbers
in this table are independently reproducible from the cleaned bench;
the previous 1.000/0.98/0.82 claim from `runs/2026-05-10-opus-r3/` is
superseded and should not be cited.**

---

## What this actually says

There are three honest findings here, in the order a reviewer should
care about:

### 1. DPM is the only condition that never smuggles invalidated state

| condition | stale escapes / stale-signal rows | rate |
|---|---|---|
| raw_oracle | 3 / 3 | 100% |
| rolling_summary | 2 / 3 | 67% |
| **dpm_phase3_checkpoint** | **0 / 3** | **0%** |

(stale-signal rows = `correction_safety` test_kind on the
`correction-heavy-session` fixture × 3 repeats. Only that fixture has a
declared typed `CorrectionDirective` — Fix B replaced the substring
detector that previously phantom-fired on long-real-session.)

Raw oracle's memory IS the literal event log, so the invalidated phrase
appears in memory by definition — 100% rate is structural. Rolling's
summarizer manages to filter the invalidated phrase one time out of
three; the other two times it propagates. **DPM's gate refuses on every
correction-detected cell, and the post-projection guard verifies the
fallback projection is clean — 0% propagation across all repeats.**

The asymmetry comes from the substrate primitive: DPM has a typed
`CorrectionDirective` (runtime data, fixture-declared, not pulled from
the hidden scoring rubric — that distinction was the validity fix
series). Rolling has no equivalent column on any cell.

### 2. Quality: raw oracle wins, DPM loses to rolling

| condition | mean decision_score | when it shines | when it doesn't |
|---|---|---|---|
| raw_oracle | 0.718 | short sessions, tool-call probes | `handoff-session` (0.000), some long-session probes |
| rolling_summary | 0.560 | short sessions | long sessions, correction-heavy |
| **dpm_phase3_checkpoint** | **0.486** | short sessions | long sessions, handoff |

This is the result the previous run hid: **DPM compresses memory more
aggressively than rolling, so it loses more information.** A 1338-char
projection over a 633-event session loses topic-token recall (e.g.
long-real-session DPM scored 0/0/0 across 3 repeats; raw scored 0.55
mean on the same probe).

The earlier 1.000 claim came from leaking `must_include=[paper, perf,
suggestion]` into the prompt — the model was being told what tokens to
emit. With the leak closed, DPM has to recover those tokens from its
1338-char projection and frequently fails.

This is not a DPM-is-bad result; it's the correct framing of the
substrate-vs-quality tradeoff. DPM trades raw quality for revocation,
audit, and constant-size memory.

### 3. Cost asymmetry survives the rerun

| condition | mean calls/cell | mean wall_ms/cell | mean input_tokens/cell |
|---|---|---|---|
| raw_oracle | 1.0 | (see report) | ~50K |
| dpm_phase3_checkpoint | 2-3 | 8-15K | ~100K |
| rolling_summary | 12-16 | 100K-150K | ~50K |

Versus rolling, DPM is still ~5-7× fewer calls and ~10× faster wall.
This claim doesn't depend on what was in the prompts; it survives the
validity fix unchanged.

---

## Per-cell breakdown

| case | raw_oracle | rolling | DPM |
|---|---|---|---|
| short-session-next-intent | **1.000** | **1.000** | **1.000** |
| tool-heavy-session | **1.000** | **1.000** | **1.000** |
| long-session-context-retention | **1.000** | 0.500 | 0.167 |
| correction-heavy-session × correction_safety | 0.750 | 0.750 | 0.750 |
| long-real-session | 0.556 | 0.111 | 0.000 |
| handoff-session | 0.000 | 0.000 | 0.000 |

Notes:

- On `short-session` and `tool-heavy`, all three conditions ace the
  probe. These are easy fixtures.
- On `correction-heavy`, all three score 0.75 — same per-cell mean.
  The DPM advantage there is *not* on quality, it's on safety: only
  DPM has a typed retraction primitive, and stale-escape rate proves
  it.
- On `long-real-session`, raw_oracle wins by a lot. Rolling and DPM
  both compress and lose information; DPM compresses harder, loses
  more.
- `handoff-session` is impossible for everyone — the probe wants a
  specific 25-char substring (`does not depend on my private logs`)
  that the model has to predict from context. Loose-rubric cases
  would score better here.

---

## Audit gate (Phase 3 invariant)

| metric | value |
|---|---|
| dpm_rows | 18 |
| gate accept | 15 |
| gate refuse | 3 |
| audit_pass | 15 |
| correction_emitted | 3 |

**3 refuses, all on `correction-heavy-session` × 3 repeats** — exactly
the fixture with a typed `CorrectionDirective`. Down from 18 refuses on
the r3 run; the missing 15 phantom refuses came from the substring
detector firing on roadmap-doc events that contained the literal word
"correction." That detector is gone (Fix B).

Manifest fingerprint and audit-certificate fields on these Python rows
are SHA-256 hashes computed at row-emit time
(`memory_agents.sha256_hex`), mirroring substrate semantics. The
substrate's actual BLAKE3 ledger (`LocalFilesystemAuditLedger`) is
exercised by `phase3_substrate_smoke.cc`; the Python matrix does not
flow rows through it.

---

## Reproduce

```bash
BENCH_USE_ANTHROPIC=1 python tools/benchmarks/dpm_phase3_bench/run_phase3_bench.py \
  --fixtures tools/benchmarks/dpm_phase3_bench/fixtures/real_sessions \
  --conditions raw_oracle,rolling_summary,dpm_phase3_checkpoint \
  --budget_chars 1338 \
  --limit_cases 6 \
  --repeat 3 \
  --model_id claude-opus-4-7 \
  --output runs/$(date -u +%Y-%m-%dT%H%M%SZ)-opus-clean.jsonl
```

Budget: ~$50-60 on Anthropic Opus 4.7. Wall: ~30-45 min on EC2.

---

## Files

- [`results.jsonl`](results.jsonl) — 54 rows, all scored
- [`report/phase3_handoff_report.md`](report/phase3_handoff_report.md)
- [`report/summary.json`](report/summary.json)
- 4 SVG charts under `report/`
- 2 example writeups under `report/examples/`

The pre-fix runs (`runs/2026-05-10-opus/`, `runs/2026-05-10-opus-postfix/`,
`runs/2026-05-10-opus-r3/`) are preserved with SUPERSEDED banners on their
HEADLINE files.
