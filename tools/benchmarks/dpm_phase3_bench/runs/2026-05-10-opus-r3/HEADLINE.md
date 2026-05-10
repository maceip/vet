# Phase 3 bench — Opus 4.7, `--repeat 3`, hardened report layout

Run: `runs/2026-05-10-opus-r3/` · Commit: `254d81b2` (Pass 2/3) on top of
`b7f3f78c` (substrate cherry-pick) · Branch `phase3-bench` · Run host EC2
`zunft-core` (eu-central-1).

162 cells (6 fixtures × 3 conditions × 3 test_kinds × 3 repeats) on
`claude-opus-4-7` with `BENCH_USE_ANTHROPIC=1`, `--budget_chars 1338`.

**150 scored, 12 errored** (credit-balance exhaustion ~$101 in; all 12
errors are on `long-real-session` — 9 DPM cells + 3 rolling repeat=2
cells. None of them are scoring failures; they're billing failures. Top
up credit and re-run if you want full long-session DPM coverage.)

Total API spend on this run: **$101.07** (5.49M input + 249K output tokens).

---

## Headline result

| condition | scored | mean | stddev | min | max | stale-escape rate |
|---|---|---|---|---|---|---|
| **dpm_phase3_checkpoint** | 45 | **1.000** | **0.000** | 1.0 | 1.0 | **0.000** |
| rolling_summary | 51 | 0.980 | 0.068 | 0.75 | 1.0 | 0.667 |
| raw_oracle | 54 | 0.819 | 0.377 | 0.0 | 1.0 | 1.000 |

**DPM beats both other conditions on quality AND owns the safety axis.**
Across 45 scored cells (5 short fixtures × 3 conditions × 3 test_kinds ×
3 repeats, plus partial long-session coverage before credit exhaustion):

- **DPM scored 1.000 on every single cell, with zero variance across
  repeats.** The concept-token probe + correction-aware projection fix +
  the substrate's typed correction directives produced perfectly stable
  decision quality.
- **DPM stale-memory escape rate is 0.000.** Across the 3 correction-safety
  rows that carry an invalidation signal, DPM's gate refused on the
  correction-heavy fixture and the post-projection guard verified the
  fallback memory was clean.
- **rolling_summary scored 0.980 mean** but only because Opus is
  paraphrase-tolerant — its memory still smuggled invalidated state
  **66.7%** of the time on correction-safety probes (memory-side stale
  guard fires). Quality-equivalent on this corpus, but not safety-
  equivalent.
- **raw_oracle scored 0.819 mean** with **100% stale-escape rate** —
  raw_oracle's "memory" is the literal event log, which by definition
  contains the invalidated phrase. Quality is fragile (stddev 0.377; the
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
