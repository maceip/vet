# Bench Output Schema

One JSON object per line. The driver appends; readers should not assume
order beyond "rows for the same (condition, trajectory, repeat) tuple
appear contiguously."

## Fields

### Configuration

| Field | Type | Required | Notes |
|---|---|---|---|
| `schema_version` | int | yes | Currently `1`. Bump on breaking format changes. |
| `mock` | bool | optional | `true` when the row is synthesized; never set by the production driver. The plotter refuses input that mixes `mock=true` and `mock=false` rows unless `--allow_mock` is passed. |
| `condition` | string | yes | One of `rolling_summary_baseline`, `summarization_baseline`, `dpm_projection`, `dpm_checkpoints`, `dpm_checkpoints_prefix_cached`, `dpm_checkpoints_handoff`. |
| `trajectory_chars` | int | yes | Length of the synthetic trajectory in characters. |
| `memory_budget_chars` | int | yes | DPM projection memory budget. The compression ratio that drives the cliff is `trajectory_chars / memory_budget_chars`. |
| `repeat_idx` | int | yes | 0-based repeat index within (condition, trajectory_chars, memory_budget_chars). |
| `seed` | int | yes | Pinned to 20260420 for the headline sweep. |
| `kv_dtype` | string | yes | One of `fp16`, `int8_per_token`. |
| `kv_dtype_policy_replay_safe` | bool | yes | Mirrors `KvDtypePolicy.require_replay_safe` at the time of measurement. |
| `model_class` | string | yes | One of `dense_mha`, `gqa`, `mqa`, `mla`, `sliding_window`. |

### Decision-alignment axes

The paper's result is **axis-specific**; a single composite score can
hide the exact failure mode reviewers care about. Producers should fill
all four axes; the composite is optional and derived.

| Field | Type | Required | Notes |
|---|---|---|---|
| `frp` | float | optional | Factual / Required-anchor Precision in [0, 1]. Omitted when the axis is judge-only or unavailable. |
| `rcs` | float | optional | Reasoning Coherence Score in [0, 1]. Often omitted before the external judge pass. |
| `eda` | float | optional | End-to-end Decision Accuracy in [0, 1] against the deterministic label. |
| `crr` | float | optional | Compliance Reconstruction in [0, 1]. |
| `decision_score` | float | optional | Four-axis mean, emitted only when all four axes have been scored. |
| `deterministic_score` | float | optional | Mean over locally scored deterministic axes, usually FRP/EDA/CRR for corpus rows before judge scoring. |
| `scored_axis_count` | int | optional | Number of axes scored locally. |
| `pending_judge_axes` | string | optional | Comma-separated axes waiting for an external judge, e.g. `rcs`. |
| `evidence_lane` | string | optional | `quality`, `ops`, or `audit`; used to keep paper-fidelity charts separate from checkpointing and handoff charts. |
| `claim_tested` | string | optional | Reader-facing claim label for the row, e.g. `paper_dpm_projection` or `checkpoint_prefix_cached_resume`. |

### Provenance

These fields make a row replay-traceable. Production runs are expected
to populate all of them; the plotter tolerates absence (older JSONL
files) but the audit story leans on them being present.

| Field | Type | Required | Notes |
|---|---|---|---|
| `architecture_tag` | string | yes | The rig's architecture, e.g. `arm64-hexagon-int8`. |
| `manifest_hash` | string | yes | Hex BLAKE3 of the canonical CheckpointAbi for this run. |
| `model_artifact_hash` | string | yes | Hex BLAKE3 of the model bundle the driver loaded. |
| `runtime_version` | string | yes | `litertlm-<version>` string. |
| `config_hash` | string | yes | Hex BLAKE3 of the YAML config that drove the run. |
| `git_sha` | string | yes | 40-char git SHA of the runtime build. |
| `dirty_tree` | bool | yes | `true` when the build was made with uncommitted changes; results from a dirty tree should be treated with suspicion. |
| `hostname` | string | yes | Hostname (or pseudonym) of the host running the bench. |
| `os` | string | yes | OS name + version, e.g. `Linux 6.8.0`, `Darwin 24.4.0`. |
| `cpu_model` | string | yes | CPU model string from `/proc/cpuinfo` or `sysctl machdep.cpu.brand_string`. |
| `accelerator_id` | string | optional | Free-form id of the accelerator (NPU / GPU / ANE) when present. |

### Wall-clock timers

| Field | Type | Required | Notes |
|---|---|---|---|
| `wall_clock_decision_ms` | float | yes | End-to-end milliseconds from request to decision. |
| `wall_clock_append_p50_us` | float | yes | Median wall-clock of an Append against the live event log. |
| `wall_clock_append_p99_us` | float | yes | p99 wall-clock of Append (captures fsync tail). |
| `wall_clock_checkpoint_put_ms` | float | optional | Time to durably commit a checkpoint blob; absent when no checkpoint occurred during this run. |
| `wall_clock_thaw_ms` | float | optional | Time from `Get` to live KV cache; absent on cold sessions. |
| `wall_clock_memory_build_ms` | float | optional | Time spent building memory before the final decision call. For DPM this is projection; for `rolling_summary_baseline` it is N summary updates. |

### Disk and KV size

| Field | Type | Required | Notes |
|---|---|---|---|
| `disk_bytes_session_total` | int | yes | Total on-disk footprint at end of run (event log + sidecars + checkpoint blobs + DAG nodes). |
| `disk_bytes_event_log` | int | yes | Just the event log file. |
| `disk_bytes_checkpoint_blobs` | int | yes | Sum of `<body_hash>.dpmpayload` files. |
| `kv_bytes_per_1024_tokens` | int | optional | The transport-encoded KV size; absent for `summarization_baseline`. |
| `must_refill_from_log` | bool | optional | `true` if the run was forced through re-prefill due to a cross-architecture mismatch or replay-safety policy rejection; absent when no thaw was attempted. |
| `tamper_test` | object | optional | Adversarial-audit row. See below. |

### `tamper_test` sub-schema (Chart D rows only)

```json
{
  "tamper_test": {
    "scenario": "manifest_hash_mismatch_artifact_hash",
    "tampered_field": "model.artifact_hash",
    "expected_manifest_hash": "<hex>",
    "actual_manifest_hash": "<hex>",
    "thaw_decision": "must_refill_from_log",
    "reason": "manifest digest mismatch"
  }
}
```

Scenario values: `clean`, `manifest_hash_mismatch_architecture_tag`,
`manifest_hash_mismatch_artifact_hash`, `cross_tenant_inject`,
`malformed_kv_payload`. The driver writes one row per scenario plus the
`clean` baseline for visual contrast.

## Versioning

`schema_version` is the only contract guarantee. A reader that does not
recognize the version should refuse to plot. The plot script asserts
`schema_version == 1` on every input row.

## Mock-vs-real isolation

`plot.py` rejects input that contains both `mock=true` and `mock=false`
rows unless `--allow_mock` is set. A stale mock row left over from
layout review can otherwise silently contaminate a real-hardware chart;
the strict default is the correct posture for any chart that is going
into a writeup or screenshot.
