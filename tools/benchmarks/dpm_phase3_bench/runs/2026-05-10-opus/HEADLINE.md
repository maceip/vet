# Phase 3 bench — second run on a real LLM, this time Opus 4.7

Run id: `2026-05-09T234038Z-phase3` (Anthropic-backed) · Commit: `1b17c4c3`
Branch: `phase3-bench` · Run host: EC2 r7a.4xlarge (`zunft-core`, eu-central-1)

Fixtures: 6 redacted real-session golden fixtures (5-fixture corpus + the
`long-real-session` codex rollout, 633 visible events).
Conditions: `raw_oracle`, `rolling_summary`, `dpm_phase3_checkpoint`.
Test kinds: `decision`, `handoff`, `correction_safety`.
Budget: 1338 chars · Repeats: 1 · Model: `claude-opus-4-7` (1M context, `AnthropicModelAdapter`).

**54 rows committed, 0 errored, 0 skipped.** The original Haiku run had 51 scored
+ 1 errored + 3 skipped (long-DPM hit the 200K-token cap; raw_oracle on long
fixtures was skipped as `skipped_context_too_large`). At 1M context the errored
row goes away, and after lifting the runner's hardcoded 200K-char ceiling for
raw_oracle (commit `1b17c4c3`) so do the skips.

Total API spend: **$65.45** (3.78M input tokens × $15/M + 116K output tokens × $75/M).

---

## What changed against the Haiku baseline

The headline metric was "rolling-summary escapes a corrected fact; DPM refuses."
On Haiku 4.5 this landed cleanly: rolling escape rate 1.0, DPM 0.0.
**On Opus 4.7 it does not replicate.** Opus is good enough at instruction-
following on the correction-heavy `correction_safety` probe that rolling-summary
no longer propagates the invalidated `"s3 transport is the contribution"`
claim into its answer.

| metric | Haiku 4.5 | Opus 4.7 |
|---|---|---|
| rolling stale escape (correction_safety) | 1/1 = **1.0** | 0/1 = **0.0** |
| dpm stale escape (correction_safety) | 0/1 = 0.0 | 0/2 = 0.0 |
| raw_oracle stale escape (correction_safety) | 0/1 = 0.0 | 0/1 = 0.0 |
| rolling mean decision_score | 0.889 | **0.944** |
| dpm mean decision_score | 0.800 | **0.833** |
| raw_oracle mean decision_score | 1.000 | **0.778** (long sessions now in scope) |

The right reading: **the v1 stale-escape headline was a Haiku-quality finding,
not a substrate finding.** Stronger models close that specific quality gap on
this corpus. To produce a model-quality-independent stale-escape signal, we'd
need either harder fixtures (longer correction chains, more adversarial
re-injection probes) or a higher-pressure decision rubric. That work is for v2.

What does survive the model swap is the substrate-level result:

---

## Substrate-level result (still holds on Opus)

### Audit gate fires correctly

| metric | value |
|---|---|
| dpm rows | 18 / 18 |
| `gate_may_use=true` (audit verdict `pass`) | 12 |
| `gate_may_use=false` (audit verdict `correction_emitted`) | 6 |
| rows with real `audit_certificate_id` (BLAKE3) | 18 / 18 |
| rows with real `checkpoint_manifest_hash` | 18 / 18 |

The 6 refusals are the 3 correction-heavy cells + 3 long-real-session cells —
exactly where blocking corrections exist in the event log. The gate refused on
those and accepted on the other 12. **Rolling-summary has no equivalent column
on any cell** — `audit_verdict`, `gate_may_use`, `checkpoint_manifest_hash`,
`audit_certificate_id` are correctly null, not zero. That's a structural
property the model swap cannot affect.

### One real stale escape, on a sidebar test_kind

The single `stale_memory_escape=true` row in this run is on
`dpm_phase3_checkpoint × handoff × correction-heavy-session`.
Score: 0.75 (partial credit). Gate: `gate_may_use=false`.
Audit verdict: `correction_emitted`. Notes verbatim:

> `forbidden propagated into answer: 'transport as the main result'`

The gate refused — but the agent's fallback path (re-projection from raw
events instead of the audited checkpoint) still produced an answer that
contained the forbidden phrase. This is a real finding: gate refusal alone
does not guarantee the fallback can't reintroduce stale content. **The
correction barrier needs to apply to the fallback path too, not just to the
checkpoint loader.** Filed as a v2 substrate concern.

---

## Cost asymmetry

| condition | rows | mean model_calls | mean input_tokens | mean wall_ms |
|---|---|---|---|---|
| raw_oracle | 18 | 1.0 | 54,830 | 4,072 |
| dpm_phase3_checkpoint | 18 | 2.3 | 88,285 | 12,553 |
| rolling_summary | 18 | **16.0** | 66,924 | **149,672** |

vs rolling-summary, DPM is **7× fewer calls, 12× faster wall time**. DPM uses
slightly *more* input tokens than rolling on this Opus run because the
single-call DPM projection on `long-real-session` renders ~200K tokens in one
shot, whereas rolling spreads its work across 16 small calls. **This inverts
when hierarchical projection (`Level0` + `DeltaAppend`, already in the C++
substrate) is wired into the Python agent** — projection becomes 2 cheap
calls per cell rather than 1 large call, and the per-call cost drops far
below rolling's compounding overhead. Until that lands, the cost story for
Opus is "same input volume as rolling, much fewer calls and far faster."

raw_oracle's mean decision quality drops from 1.000 (Haiku, where 3 long
cells were skipped) to **0.778** because long-session probes are now in
scope and the model fails to find the answer in a 324K-token full event log
(`long-real-session × decision` scored 0.0). **The "raw oracle is the
ceiling" framing is also Haiku-shaped:** with long sessions in scope, raw
oracle is no longer a ceiling, it's a needle-in-haystack failure mode.

---

## Reproduce

```bash
# Anthropic-backed full matrix (Opus 4.7, ~$65)
BENCH_USE_ANTHROPIC=1 python tools/benchmarks/dpm_phase3_bench/run_phase3_bench.py \
  --fixtures tools/benchmarks/dpm_phase3_bench/fixtures/real_sessions \
  --conditions raw_oracle,rolling_summary,dpm_phase3_checkpoint \
  --budget_chars 1338 \
  --test_kinds decision,handoff,correction_safety \
  --limit_cases 6 \
  --model_id claude-opus-4-7 \
  --output runs/$(date -u +%Y-%m-%dT%H%M%SZ)-opus.jsonl
```

The targeted top-up for the 3 raw_oracle long-session cells (the merge that
brought this run from 51 scored + 3 skipped → 54 scored, 0 skipped) is
preserved as `raw_oracle_long_topup.jsonl`. The pre-merge state is preserved
as `results.jsonl.pre-merge`. Both are committed alongside `results.jsonl` so
the merge is reproducible — `results.jsonl` is the headline data set; the
other two are the audit trail of how it was constructed.

## Files

- [`results.jsonl`](results.jsonl) — 54 schema-validated rows, all scored
- [`results.jsonl.pre-merge`](results.jsonl.pre-merge) — pre-topup snapshot (51 scored + 3 skipped)
- [`raw_oracle_long_topup.jsonl`](raw_oracle_long_topup.jsonl) — 3 raw_oracle × long-session top-up rows
- [`report/phase3_handoff_report.md`](report/phase3_handoff_report.md) — rendered report
- [`report/summary.json`](report/summary.json) — aggregated tables
- [`report/chart_decision_accuracy.svg`](report/chart_decision_accuracy.svg)
- [`report/chart_stale_memory_escape.svg`](report/chart_stale_memory_escape.svg)
- [`report/chart_audit_gate.svg`](report/chart_audit_gate.svg)
- [`report/chart_cost_latency.svg`](report/chart_cost_latency.svg)
- [`report/examples/dpm_gate_case.md`](report/examples/dpm_gate_case.md)
- [`report/examples/rolling_escape_case.md`](report/examples/rolling_escape_case.md) — stub (no rolling escape on Opus)
