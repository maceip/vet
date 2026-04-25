# Bench Output Schema

One JSON object per line. The driver appends; readers should not assume
order beyond "rows for the same (condition, trajectory, repeat) tuple
appear contiguously."

## Fields

| Field | Type | Required | Notes |
|---|---|---|---|
| `schema_version` | int | yes | Currently `1`. Bump on breaking format changes. |
| `mock` | bool | optional | `true` when the row is synthesized; never set by the production driver. |
| `condition` | string | yes | One of `summarization_baseline`, `dpm_projection`, `dpm_checkpoints`, `dpm_checkpoints_prefix_cached`. |
| `trajectory_chars` | int | yes | Length of the synthetic trajectory in characters. |
| `memory_budget_chars` | int | yes | DPM projection memory budget. |
| `repeat_idx` | int | yes | 0-based repeat index within (condition, trajectory_chars, memory_budget_chars). |
| `seed` | int | yes | Pinned to 20260420 for the headline sweep. |
| `architecture_tag` | string | yes | The rig's architecture, e.g. `arm64-hexagon-int8`. |
| `manifest_hash` | string | yes | Hex BLAKE3 of the model artifact bundle the driver loaded. |
| `runtime_version` | string | yes | `litertlm-<version>` string. |
| `kv_dtype` | string | yes | One of `fp16`, `int8_per_token`. |
| `kv_dtype_policy_replay_safe` | bool | yes | Mirrors `KvDtypePolicy.require_replay_safe` at the time of measurement. |
| `model_class` | string | yes | One of `dense_mha`, `gqa`, `mqa`, `mla`, `sliding_window`. |
| `decision_score` | float | yes | Composite of FRP × RCS × CRR × EDA from the paper, in [0, 1]. |
| `wall_clock_decision_ms` | float | yes | End-to-end milliseconds from request to decision. |
| `wall_clock_append_p50_us` | float | yes | Median wall-clock of an Append against the live event log. |
| `wall_clock_append_p99_us` | float | yes | p99 wall-clock of Append (captures fsync tail). |
| `wall_clock_checkpoint_put_ms` | float | optional | Time to durably commit a checkpoint blob; absent when no checkpoint occurred during this run. |
| `wall_clock_thaw_ms` | float | optional | Time from `Get` to live KV cache; absent on cold sessions. |
| `disk_bytes_session_total` | int | yes | Total on-disk footprint at end of run (event log + sidecars + checkpoint blobs + DAG nodes). |
| `disk_bytes_event_log` | int | yes | Just the event log file. |
| `disk_bytes_checkpoint_blobs` | int | yes | Sum of `<hash>.dpmckpt` files. |
| `kv_bytes_per_1024_tokens` | int | optional | The transport-encoded KV size; absent for `summarization_baseline`. |
| `must_refill_from_log` | bool | optional | `true` if the run was forced through re-prefill due to a cross-architecture mismatch or replay-safety policy rejection; absent when no thaw was attempted. |
| `tamper_test` | object | optional | Adversarial-audit row. See below. |

### `tamper_test` sub-schema (Chart D rows only)

```json
{
  "tamper_test": {
    "scenario": "manifest_hash_mismatch",
    "tampered_field": "producer.architecture_tag",
    "expected_manifest_hash": "<hex>",
    "actual_manifest_hash": "<hex>",
    "thaw_decision": "must_refill_from_log",
    "reason": "manifest digest mismatch"
  }
}
```

`scenario` is one of `manifest_hash_mismatch`, `cross_tenant_inject`,
`malformed_kv_payload`. The driver writes one row per scenario and one
"clean" baseline row for visual contrast.

## Versioning

`schema_version` is the only contract guarantee. A reader that does not
recognize the version should refuse to plot. The plot script asserts
`schema_version == 1` on every input row.
