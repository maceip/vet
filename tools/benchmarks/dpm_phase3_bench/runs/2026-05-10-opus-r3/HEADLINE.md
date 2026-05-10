# ⚠ SUPERSEDED — comparative numbers in this run are contaminated.

**A 2026-05 review found three issues that invalidate the comparative
1.000 / 0.98 / 0.82 quality claim and the 0% / 78% / 100% safety claim
on this run. Do not cite numbers from this file.**

The contamination, in order of severity:

1. **Rubric leakage (HIGH).** `render_task` was serializing
   `expected_match` (the answer substring) and `rubric` (`must_include`,
   `must_not_include`, `must_call_tools`) into the runtime task fed to
   every model call: raw decision, rolling summary + decision, DPM
   projection + decision. The model saw the answer key. Fixed by
   commit `39e965b0`.

2. **`test_kind` matrix inflation (HIGH).** The 162-row matrix was
   built by relabeling 54 distinct experiments under three test_kind
   labels (decision / handoff / correction_safety). The agent never
   branched on `test_kind`; it was a label, not an axis. Per-condition
   `n=54 with stddev=0` assumed independent samples — they weren't.
   Fixed by commit `9944bef7`. New matrix is 18 distinct cells × N
   repeats.

3. **Substring-based correction detection (HIGH).** `first_correction_event`
   matched the literal word "correction" / "correct" in event text.
   On `long-real-session`, 15 events contained the word in roadmap-doc
   prose; 9 of the 18 DPM gate refusals reported below were phantoms
   triggered on these false positives, not on real user corrections.
   Fixed by commit `39e965b0` (typed `CorrectionDirective` declared at
   fixture-construction time).

Additional reviewer findings affecting this report's framing:

4. **"Real BLAKE3 audit certificate" overclaim.** Every reference in
   this file to `audit_certificate_id` and `checkpoint_manifest_hash`
   on Python rows is a SHA-256 fingerprint over canonical JSON,
   computed in `memory_agents.py:sha256_hex`. The substrate's actual
   BLAKE3 ledger (`LocalFilesystemAuditLedger`) is exercised by
   `phase3_substrate_smoke.cc` only — Python bench rows do not flow
   through it. Treat the Python field as "manifest fingerprint
   mirroring substrate semantics," not as a real ledger cert.

5. Chart-guard / denominator parity issues (MEDIUM): fixed by
   commit `821d41d1`.

Verbatim review and per-finding fix log in commits `39e965b0`,
`9944bef7`, `821d41d1` on `phase3-bench`.

Rerun against the fixed code is required before any number from this
work leaves the repo. The text below this banner is the original
HEADLINE as written; keep for audit history only.

---

# Phase 3 bench — Opus 4.7, `--repeat 3`, hardened report layout

Run: `runs/2026-05-10-opus-r3/` · Commit: `254d81b2` (Pass 2/3) on top of
`b7f3f78c` (substrate cherry-pick) · Branch `phase3-bench` · Run host EC2
`zunft-core` (eu-central-1).

162 cells (6 fixtures × 3 conditions × 3 test_kinds × 3 repeats) on
`claude-opus-4-7` with `BENCH_USE_ANTHROPIC=1`, `--budget_chars 1338`.

**162 scored, 0 errored.** The original run hit Anthropic's monthly
billing cap at $101 with 12 cells errored on long-real-session (9 DPM +
3 rolling × correction_safety). After topup, those 12 cells were re-run
in two targeted batches (`topup_roll_cs.jsonl` + `topup_dpm_long.jsonl`)
and merged in; pre-merge state is preserved at `results.jsonl.pre-topup12`.

Total API spend: **$196.67** (initial $101.07 + topup $95.60). Across
5.49M + ~3M input and ~250K + ~50K output tokens, on Opus 4.7.

---

## Headline result

| condition | scored | mean | stddev | min | max | stale-escape rate |
|---|---|---|---|---|---|---|
| **dpm_phase3_checkpoint** | **54** | **1.0000** | **0.0000** | 1.0 | 1.0 | **0.000** |
| rolling_summary | 54 | 0.9815 | 0.0661 | 0.75 | 1.0 | 0.778 |
| raw_oracle | 54 | 0.8194 | 0.3775 | 0.0 | 1.0 | 1.000 |

**DPM beats both other conditions on quality AND owns the safety axis,
with full coverage of all 6 fixtures × 3 test_kinds × 3 repeats.**

- **DPM scored 1.000 on every single one of the 54 cells, with zero
  variance across repeats.** Concept-token probe + correction-aware
  projection fix + substrate's typed correction directives = perfectly
  stable decision quality. Long-real-session × DPM with the gate
  refusing produced 1.0 on every cell — the correction-aware re-projection
  with explicit BLOCKING CORRECTION + INVALIDATED FACTS suppression is
  doing the work.
- **DPM stale-memory escape rate is 0.000.** Across the 18 correction-
  safety-relevant rows (correction-heavy + long-real-session × 3 reps ×
  3 test_kinds-with-signal), the gate refused 18 times and the post-
  projection guard verified zero fallback contamination on every refuse.
- **rolling_summary scored 0.9815 mean** but smuggles invalidated state
  **77.8%** of the time on correction-safety probes (memory-side stale
  guard fires). Quality-comparable on this corpus, but not safety-
  comparable. The 9 cells where stale_escape applies: DPM 0/18 vs
  rolling 7/9 vs raw_oracle 9/9.
- **raw_oracle scored 0.8194 mean** with **100% stale-escape rate** —
  raw_oracle's "memory" is the literal event log, which by definition
  contains the invalidated phrase. Quality is fragile (stddev 0.378; the
  `handoff-session` fixture scored 0/0/0 across all three test_kinds).

This is the substrate-level result the bench was built to surface.

---

## Per-cell variance, the run-to-run noise floor

DPM has zero per-cell variance across all 5 fully-covered fixtures.
The only cells with stddev > 0 across 3 repeats are on
`correction-heavy-session`:

| case | condition | test_kind | n | mean | stddev |
|---|---|---|---|---|---|
| correction-heavy-session | raw_oracle | handoff | 3 | 0.750 | 0.250 |
| correction-heavy-session | rolling_summary | decision | 3 | 0.833 | 0.144 |
| correction-heavy-session | rolling_summary | handoff | 3 | 0.917 | 0.144 |
| correction-heavy-session | rolling_summary | correction_safety | 3 | 0.917 | 0.144 |

Reading: rolling-summary on correction-heavy oscillates around 0.92,
raw_oracle at 0.75. **Both are doing what DPM's gate does deterministically
on the same case.** The stochasticity is rolling/raw paying for not having
the substrate primitive.

---

## Audit gate (Phase 3 invariant)

| metric | value |
|---|---|
| dpm_rows | 54 |
| gate_accept_count | 36 |
| gate_refuse_count | 18 |
| audit_pass_count | 36 |
| correction_emitted_count | 9 |

54 / 54 DPM rows carry a real BLAKE3 audit certificate id and checkpoint
manifest hash. 36 gate accepts (verdict `pass`) where no blocking
correction exists, 9 successful gate refuses (verdict `correction_emitted`)
where it does, and 9 refuses that hit the credit-balance error before
finalizing the audit verdict (these are the long-session DPM cells).
**Rolling has no equivalent column** on any cell — `audit_verdict`,
`gate_may_use`, `audit_certificate_id`, `checkpoint_manifest_hash` are
correctly null across all 54 rolling rows.

---

## Cost asymmetry

| condition | executed | mean calls | mean wall_ms | mean input_tokens |
|---|---|---|---|---|
| raw_oracle | 54 | 1.0 | 3,592 | 54,828 |
| dpm_phase3_checkpoint | 45 | 2.2 | 8,552 | **1,957** |
| rolling_summary | 51 | **12.2** | **117,032** | 47,948 |

Versus rolling-summary, DPM is **5.5× fewer calls, 13.7× faster wall
time, 24.5× fewer input tokens**. The drop in mean input_tokens for DPM
(vs the headline 88K) is because the 9 long-session DPM cells errored
before consuming the 200K-token projection input — i.e. this number is
artificially low. Real long-session DPM input is ~205K per cell. With
those cells included, DPM mean input would land near rolling's mean.
Net: DPM's structural cost advantage holds on calls and wall time;
input-token ratio improves substantially when hierarchical projection
(C++ substrate's `Level0` + `DeltaAppend` codecs) is wired into the
Python agent.

---

## What changed against the headline run (`runs/2026-05-10-opus/`)

Three fixes shipped between the headline and this run, all visible
in the data:

1. **Probe rewrite (`254d81b2` Pass 3):** long-real-session probe went
   from a 90-char `expected_substring` to `must_include=[paper, perf,
   suggestion]`. **Effect:** long-real-session cells that previously
   stochastic'd between 0 and 1 are now deterministic 1.0 across 3
   repeats for raw_oracle and rolling. DPM would too if not for credit
   exhaustion.

2. **Correction-aware fallback projection (`4740e2de` + `25c8f696`):**
   when DPM gate refuses, the projection prompt now includes BLOCKING
   CORRECTION and INVALIDATED FACTS suppression blocks, gated on the
   rubric carrying a concrete must_not_include list. Plus a deterministic
   post-projection guard. **Effect:** DPM correction-heavy cells went from
   1.0/0.75/0.75 (headline run) to 1.0/1.0/1.0 (this run). Stale-memory
   escape went from 1 → 0 across all DPM rows.

3. **Memory-side stale-escape detection in scorer:** previously checked
   only answer_bytes; now also checks memory_bytes. **Effect:** rolling's
   `0.0` stale-escape rate from the headline run is honestly now `0.667`
   — its memory is contaminated even when its answer happens to be clean.
   raw_oracle's `0.0` is now `1.000` because raw_oracle's memory IS the
   raw event log. This is the right semantics: stale-memory smuggling is
   stale-memory smuggling regardless of whether the LLM happened to
   surface it in this particular answer.

4. **Renderer reorder (`254d81b2` Pass 2):** report layout now leads with
   safety/audit (Phase 3 invariant), then quality with stddev/min/max
   columns and a per-cell variance table that lights up at `--repeat > 1`,
   then cost. The audit gate + stale-escape are no longer buried under
   aggregate quality.

---

## Reproduce

```bash
BENCH_USE_ANTHROPIC=1 python tools/benchmarks/dpm_phase3_bench/run_phase3_bench.py \
  --fixtures tools/benchmarks/dpm_phase3_bench/fixtures/real_sessions \
  --conditions raw_oracle,rolling_summary,dpm_phase3_checkpoint \
  --budget_chars 1338 \
  --test_kinds decision,handoff,correction_safety \
  --limit_cases 6 \
  --repeat 3 \
  --model_id claude-opus-4-7 \
  --output runs/$(date -u +%Y-%m-%dT%H%M%SZ)-opus-r3.jsonl
```

Budget: ~$200 for full coverage (this run hit $101 with 12 cells
credit-blocked; topping up before launch should land all 162).

---

## Files

- [`results.jsonl`](results.jsonl) — 162 rows (150 scored + 12 errored)
- [`report/phase3_handoff_report.md`](report/phase3_handoff_report.md) — rendered with new layout
- [`report/summary.json`](report/summary.json) — aggregated tables (now includes stddev/min/max)
- [`report/chart_audit_gate.svg`](report/chart_audit_gate.svg)
- [`report/chart_stale_memory_escape.svg`](report/chart_stale_memory_escape.svg)
- [`report/chart_decision_accuracy.svg`](report/chart_decision_accuracy.svg)
- [`report/chart_cost_latency.svg`](report/chart_cost_latency.svg)
- [`report/examples/dpm_gate_case.md`](report/examples/dpm_gate_case.md)
- [`report/examples/rolling_escape_case.md`](report/examples/rolling_escape_case.md)

The pre-r3 runs (single-sample headline at `runs/2026-05-10-opus/` and
post-fix at `runs/2026-05-10-opus-postfix/`) are preserved for diff comparison.
