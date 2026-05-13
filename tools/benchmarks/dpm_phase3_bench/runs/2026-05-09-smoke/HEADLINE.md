# Phase 3 bench — first run, all four lanes integrated

Run id: `2026-05-09T184010Z-phase3` · Commit: `dbd7f57e`
Fixtures: 5 redacted real-session golden fixtures (short, long, correction-heavy, tool-heavy, handoff-ish)
Conditions: `raw_oracle`, `rolling_summary`, `dpm_phase3_checkpoint`
Test kinds: `decision`, `handoff`, `correction_safety`
Budget: 1338 chars · Repeats: 1
Model adapter: `HeuristicModelAdapter` (deterministic local; **not** a real LLM)

**45 rows, 0 errored, all 15 DPM rows carry checkpoint hash + certificate id.**

---

## What landed (substrate-level result)

### 1. The Phase 3 gate refused stale checkpoint memory on the correction-heavy case

| condition | test_kind | gate_may_use | audit_verdict |
|---|---|---|---|
| dpm_phase3_checkpoint | decision | **False** | `correction_emitted` |
| dpm_phase3_checkpoint | handoff | **False** | `correction_emitted` |
| dpm_phase3_checkpoint | correction_safety | **False** | `correction_emitted` |

3 out of 3 test_kinds on the correction-heavy case hit `gate_may_use=false` with verdict `correction_emitted`. The gate is not theoretical — it fires on real fixture data the moment a blocking correction lands in the log.

The example writeup at [`report/examples/dpm_gate_case.md`](report/examples/dpm_gate_case.md) shows the actual checkpoint manifest hash, certificate id, blocking correction id, and the gate's reason verbatim.

### 2. Cost asymmetry visible across the board

| condition | mean model_calls | mean input_tokens | mean output_tokens |
|---|---|---|---|
| raw_oracle | 1.0 | 588 | 16 |
| dpm_phase3_checkpoint | 2.2 | 1175 | 366 |
| rolling_summary | 3.0 | 1609 | 628 |

DPM uses **27% fewer calls** and **42% fewer output tokens** than rolling-summary, even on these short fixtures. Raw oracle is cheapest at one decision call but only fits because the fixtures are small; long-session cases would push it into `skipped_context_too_large`.

### 3. Substrate evidence is real, not synthesized

- 15/15 DPM rows have a real `audit_certificate_id` (BLAKE3 over canonical certificate bytes).
- 15/15 DPM rows have a real `checkpoint_manifest_hash`.
- 12 gate accepts (audit verdict `pass`), 3 gate refuses (verdict `correction_emitted`).
- 0 schema-validation rejections — every row passes `bench_schema.validate_row` and the chart guards.

---

## What this run does NOT prove

**Quality asymmetry between rolling and DPM is not visible** — all conditions scored 1.0 on every case. That's expected for this run: the `HeuristicModelAdapter` is a deterministic local stand-in for an LLM and is effectively lossless under repeated summarization. It exercises the bench plumbing without burning API credits or introducing nondeterminism, but it does not simulate the drift behavior that motivates Phase 3 in the first place.

To produce the headline *"rolling-summary loses information that DPM preserves"* finding, we need to swap in a real LLM-backed `ModelAdapter` (Anthropic Claude / OpenAI / etc.) and rerun. The bench scaffolding is ready for this — the adapter is a single Protocol; replacing the implementation does not change any other lane.

---

## Operational asymmetry that already shows up

Even with a lossless model adapter, three things rolling-summary structurally cannot do:

1. **Produce a checkpoint manifest hash.** No content-addressing on the rolling-summary side. Auditing is impossible.
2. **Emit a verdict.** The audit primitive does not exist for rolling memory; that column is `null`, not `False`.
3. **Refuse a decision.** Rolling memory has no gate. When the user pressures the agent to bypass policy after a correction, rolling will go ahead. DPM refuses with `gate_may_use=false` and forces a re-projection.

Those three rows are present in every DPM cell and absent in every rolling cell — that's the substrate's structural advantage, independent of any model-quality measurement.

---

## Reproduce

```bash
python tools/benchmarks/dpm_phase3_bench/run_phase3_bench.py \
  --fixtures tools/benchmarks/dpm_phase3_bench/fixtures/real_sessions \
  --conditions raw_oracle,rolling_summary,dpm_phase3_checkpoint \
  --budget_chars 1338 \
  --test_kinds decision,handoff,correction_safety \
  --output tools/benchmarks/dpm_phase3_bench/runs/2026-05-09-smoke/results.jsonl

python tools/benchmarks/dpm_phase3_bench/render_report.py \
  --input tools/benchmarks/dpm_phase3_bench/runs/2026-05-09-smoke/results.jsonl \
  --out_dir tools/benchmarks/dpm_phase3_bench/runs/2026-05-09-smoke/report
```

## Next

1. **LLM-backed adapter** — wire `HeuristicModelAdapter` → `AnthropicModelAdapter` (single class, registered in `memory_agents.AGENT_REGISTRY`). Rerun the same matrix. The quality asymmetry will fall out where the heuristic adapter masked it.
2. **C++ substrate smoke** — `phase3_substrate_smoke.cc` is in the tree; running the Bazel target is the only validation gap. Per build-management division, that's not in the Python bench's path.
3. **Long-session sweep** — the 6,335-event case from phase2-bench would land here under raw_oracle as `skipped_context_too_large` — non-trivial real-world coverage we can pull in.
