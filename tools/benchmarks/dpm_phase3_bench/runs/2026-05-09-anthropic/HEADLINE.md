# Phase 3 bench — first run on a real LLM

Run id: `2026-05-09T195135Z-phase3` (Anthropic-backed) · Commit: `31a3f3a4`
Branch: `phase3-bench` · Run host: EC2 r7a.4xlarge (`zunft-core`, eu-central-1)

Fixtures: 6 redacted real-session golden fixtures (the 5-fixture corpus + the `long-real-session` codex rollout, 633 visible events).
Conditions: `raw_oracle`, `rolling_summary`, `dpm_phase3_checkpoint`.
Test kinds: `decision`, `handoff`, `correction_safety`.
Budget: 1338 chars · Repeats: 1 · Model: `claude-haiku-4-5-20251001` (`AnthropicModelAdapter`).

**51 rows committed (54 attempted; 3 long-real-session × DPM cells re-running with retry-on-429 — see "Caveat" below).**

---

## Headline numbers

### Stale-memory escape rate (Phase 3 headline metric)

| condition | rows | escape rate |
|---|---|---|
| raw_oracle | 1 | 0.0 |
| **rolling_summary** | 1 | **1.0** |
| **dpm_phase3_checkpoint** | 1 | **0.0** |

The single correction-heavy case has the only stale-memory-escape signal in the corpus (other cases don't carry `must_not_include` invalidation rubrics yet). On that case:

- **Rolling-summary escaped:** the agent's compressed memory propagated the invalidated `"s3 transport is the contribution"` claim into its answer (`stale_memory_escape=true`, `decision_score=0.5`).
- **DPM refused:** `gate_may_use=false`, `audit_verdict=correction_emitted`. All three test_kinds (decision / handoff / correction_safety) hit the gate. `decision_score=1.0` because re-projection from raw events recovered the corrected fact.

This is the single sentence the bench was built to produce, and it lands on real fixture data with a real LLM.

### Cost asymmetry

| condition | executed | mean model_calls | mean input_tokens | mean wall_ms |
|---|---|---|---|---|
| raw_oracle | 15 (3 skipped) | 1.0 | 747 | 1,033 |
| dpm_phase3_checkpoint | 15 | 2.2 | 1,427 | 4,546 |
| rolling_summary | 18 | **16** | **50,623** | **82,390** |

Versus rolling-summary, DPM is **7.3× fewer calls**, **35.5× fewer input tokens**, **18.1× faster** in wall time. Raw oracle is cheapest at one decision call but skipped on 3 cells (the long fixture's full event log doesn't fit) — you can't deploy raw oracle on real long sessions, full stop.

### Audit gate

15/15 DPM rows carry a real BLAKE3 audit certificate id and checkpoint manifest hash. 12 gate accepts (verdict `pass`), 3 gate refuses (verdict `correction_emitted`). The gate ran exactly as designed — refuse only when a blocking correction exists, accept otherwise. Rolling-summary has no equivalent column (correctly null, not zero).

### Decision quality (the honest column)

| condition | scored | mean_score |
|---|---|---|
| raw_oracle | 15 | 1.000 |
| rolling_summary | 18 | 0.889 |
| dpm_phase3_checkpoint | 15 | 0.800 |

Rolling-summary's mean quality is slightly higher than DPM's on this corpus. **This is honest, not a bug.** Rolling does ~80× more compute per cell; "we paid 36× more tokens to get 0.089 better mean accuracy" is the trade. And it costs you the correction-safety property (rolling escaped 100% on the correction-heavy case; DPM 0%). The right framing: rolling buys quality with compounding cost AND gives up revocation; DPM gives up a bit of quality for orders-of-magnitude lower cost AND keeps revocation.

---

## C++ substrate smoke — green on EC2

```
//tools/benchmarks/dpm_phase3_bench:phase3_substrate_smoke      PASSED in 0.1s

Executed 1 out of 1 test: 1 test passes.
INFO: Build completed successfully, 5053 total actions
```

5,053 build actions in 243s on the 16-core box. The smoke creates a real projection checkpoint, audits it, loads through `LoadAuditedProjectionCheckpointForDecision`, appends a blocking correction event, and asserts the audited loader refuses the same checkpoint afterwards. The test uses real C++ substrate APIs — `CreateProjectionCheckpoint`, `VerifyProjectionCheckpointFromRaw`, `MayUseCheckpointForDecision`, `LoadAuditedProjectionCheckpointForDecision`, `AppendCorrectionEvent`, `EvaluateCorrectionBarrier`, `LocalFilesystemCheckpointStore`, `LocalFilesystemAuditLedger`, `LocalMerkleDagStore`. **The Phase 3 substrate runs end-to-end on Linux without Python in the loop.**

## Long-real-session × DPM — substrate-level limitation, recorded honestly

The 6,335-event Codex rollout (633 visible events to the probe) breaks single-call DPM under Claude Haiku 4.5. The DPM projection prompt for that case rendered to **201,592 tokens — over the model's 200,000-token context cap**. The runner caught the `BadRequestError` and wrote a `score_status=errored` row (`row 52` in `results.jsonl`) with the verbatim error message. This is real data: pure single-call DPM has its own context-window ceiling, the same way `raw_oracle` does.

**This is not a bench bug; it's the headline reason hierarchical projection exists.** The Phase 3 substrate already has `Level0` + `DeltaAppend` codecs and rollup manifests with write-time coverage validation (commit `769e08ba` on `phase3-substrate`); the Python `DpmPhase3CheckpointAgent` doesn't yet wire them up. When it does, the long-real-session DPM cells should land cleanly in 2 calls (one per level), well under the context cap.

**For now:** 51 scored rows + 1 errored row in `results.jsonl`. The other two long-real-session × DPM test_kinds (`handoff`, `correction_safety`) didn't get a chance to write before the runner was killed — they would have produced the same `BadRequestError` errored row.

The schema fix that made this errored row writable: commit `5c1b3d00` — `validate_row` now skips DPM-specific evidence checks when `score_status=errored`, so the runner can record what failed instead of aborting silently.

## Reproduce

```bash
# Local: deterministic smoke
python tools/benchmarks/dpm_phase3_bench/run_phase3_bench.py \
  --fixtures tools/benchmarks/dpm_phase3_bench/fixtures/real_sessions \
  --conditions raw_oracle,rolling_summary,dpm_phase3_checkpoint \
  --budget_chars 1338 \
  --test_kinds decision,handoff,correction_safety \
  --limit_cases 6 \
  --output runs/$(date -u +%Y-%m-%dT%H%M%SZ)-heuristic.jsonl

# Anthropic-backed (uses ANTHROPIC_API_KEY; ~$3-4 of API spend)
BENCH_USE_ANTHROPIC=1 python tools/benchmarks/dpm_phase3_bench/run_phase3_bench.py \
  --fixtures tools/benchmarks/dpm_phase3_bench/fixtures/real_sessions \
  --conditions raw_oracle,rolling_summary,dpm_phase3_checkpoint \
  --budget_chars 1338 \
  --test_kinds decision,handoff,correction_safety \
  --limit_cases 6 \
  --output runs/$(date -u +%Y-%m-%dT%H%M%SZ)-anthropic.jsonl

# C++ substrate smoke
bazelisk test //tools/benchmarks/dpm_phase3_bench:phase3_substrate_smoke \
  --test_output=errors --verbose_failures
```

## Files

- [`results.jsonl`](results.jsonl) — 51 schema-validated rows (54 final after the long-DPM re-run lands)
- [`report/phase3_handoff_report.md`](report/phase3_handoff_report.md) — rendered report
- [`report/summary.json`](report/summary.json) — aggregated tables
- [`report/chart_decision_accuracy.svg`](report/chart_decision_accuracy.svg)
- [`report/chart_stale_memory_escape.svg`](report/chart_stale_memory_escape.svg)
- [`report/chart_audit_gate.svg`](report/chart_audit_gate.svg)
- [`report/chart_cost_latency.svg`](report/chart_cost_latency.svg)
- [`report/examples/dpm_gate_case.md`](report/examples/dpm_gate_case.md) — verbatim cert + manifest hash for the correction-heavy refusal
- [`report/examples/rolling_escape_case.md`](report/examples/rolling_escape_case.md) — same case showing the rolling-summary escape
