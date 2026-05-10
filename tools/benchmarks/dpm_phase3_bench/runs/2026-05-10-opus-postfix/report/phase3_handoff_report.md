# Phase 3 Handoff Report

This report compares rolling memory with DPM Phase 3 checkpointed
decision memory on audit-safe handoff after a correction.

## Run Summary

- Rows: `54`
- Cases: `6`
- Needs judge rows: `0`
- Errored rows: `0`

## Decision Quality

| condition | scored_rows | mean_decision_score |
| --- | --- | --- |
| raw_oracle | 18 | 0.778 |
| rolling_summary | 18 | 0.944 |
| dpm_phase3_checkpoint | 18 | 0.861 |

![Decision quality](chart_decision_accuracy.svg)

## Stale-Memory Escape

Lower is better. This is the Phase 3 headline metric.

| condition | rows | escape_rate |
| --- | --- | --- |
| raw_oracle | 1 | 0 |
| rolling_summary | 1 | 0 |
| dpm_phase3_checkpoint | 2 | 0 |

![Stale-memory escape](chart_stale_memory_escape.svg)

## Audit Gate

Rolling memory has no equivalent to this gate; DPM rows expose certificate
and correction evidence directly.

| metric | value |
| --- | --- |
| dpm_rows | 18 |
| gate_accept_count | 12 |
| gate_refuse_count | 6 |
| audit_pass_count | 12 |
| correction_emitted_count | 6 |

![Audit gate](chart_audit_gate.svg)

## Cost

| condition | executed_rows | skipped_or_errored | mean_model_calls | mean_wall_ms | mean_input_tokens |
| --- | --- | --- | --- | --- | --- |
| raw_oracle | 18 | 0 | 1 | 4072 | 54830 |
| rolling_summary | 18 | 0 | 16 | 149672 | 66924 |
| dpm_phase3_checkpoint | 18 | 0 | 2.333 | 10774 | 88315 |

![Cost latency](chart_cost_latency.svg)

## Examples

- [Rolling memory stale escape](examples/rolling_escape_case.md)
- [DPM audit gate case](examples/dpm_gate_case.md)
