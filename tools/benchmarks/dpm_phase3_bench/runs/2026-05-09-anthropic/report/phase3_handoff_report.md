# Phase 3 Handoff Report

This report compares rolling memory with DPM Phase 3 checkpointed
decision memory on audit-safe handoff after a correction.

## Run Summary

- Rows: `52`
- Cases: `6`
- Needs judge rows: `0`
- Errored rows: `1`

## Decision Quality

| condition | scored_rows | mean_decision_score |
| --- | --- | --- |
| raw_oracle | 15 | 1 |
| rolling_summary | 18 | 0.889 |
| dpm_phase3_checkpoint | 15 | 0.8 |

![Decision quality](chart_decision_accuracy.svg)

## Stale-Memory Escape

Lower is better. This is the Phase 3 headline metric.

| condition | rows | escape_rate |
| --- | --- | --- |
| raw_oracle | 1 | 0 |
| rolling_summary | 1 | 1 |
| dpm_phase3_checkpoint | 1 | 0 |

![Stale-memory escape](chart_stale_memory_escape.svg)

## Audit Gate

Rolling memory has no equivalent to this gate; DPM rows expose certificate
and correction evidence directly.

| metric | value |
| --- | --- |
| dpm_rows | 16 |
| gate_accept_count | 12 |
| gate_refuse_count | 4 |
| audit_pass_count | 12 |
| correction_emitted_count | 3 |

![Audit gate](chart_audit_gate.svg)

## Cost

| condition | executed_rows | skipped_or_errored | mean_model_calls | mean_wall_ms | mean_input_tokens |
| --- | --- | --- | --- | --- | --- |
| raw_oracle | 15 | 3 | 1 | 1033 | 747 |
| rolling_summary | 18 | 0 | 16 | 82390 | 50623 |
| dpm_phase3_checkpoint | 15 | 1 | 2.2 | 4546 | 1427 |

![Cost latency](chart_cost_latency.svg)

## Examples

- [Rolling memory stale escape](examples/rolling_escape_case.md)
- [DPM audit gate case](examples/dpm_gate_case.md)
