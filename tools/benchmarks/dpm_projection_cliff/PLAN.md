# Execution plan

## Phase 1 — scaffolding (this commit)

- Driver header / lib / main / tests compile against absl + the JSONL
  schema. `RunOneCell` returns `mock=true` rows so the chart pipeline
  can be exercised end-to-end without hardware.
- `plot.py` accepts `--mock` and emits five SVGs in the skeptic-resistant
  order (E → B → C → D → A).
- BUILD wired; CMake skipped because the sibling
  `tools/benchmarks/dpm_prefill_bench/` is Bazel-only.
- `bazel test //tools/benchmarks/dpm_projection_cliff/...` runs
  `dpm_projection_cliff_test.cc`: JSONL serializer round-trip, YAML
  loader scalar/list/comment/missing-file paths, `RunOneCell` mock
  determinism.

## Phase 2 — wire `RunOneCell` against the real substrate

The `RunOneCell` stub is the integration seam. Replace its body with:

1. **`summarization_baseline`** — drive the upstream rolling-summary
   path against a synthetic event log of length `trajectory_chars`.
   Score against the four-axis decision-alignment reference.
2. **`dpm_projection`** — `EventSourcedLog::Append` × N then
   `DPMProjector::Project` once. Time the projection call. No
   checkpoint store interaction.
3. **`dpm_checkpoints`** — same as `dpm_projection` plus
   `CheckpointStore::Put` at trigger boundaries (handoff, milestone,
   token threshold) and a thaw on a warm-start replay.
4. **`dpm_checkpoints_prefix_cached`** — same as `dpm_checkpoints` but
   pin the static head via `ProjectionPromptParts.cacheable_prefix`
   in the inference backend's prefix cache and time the warm path.

Per-cell timers we already need:
- `wall_clock_decision_ms` — end-to-end.
- `wall_clock_append_p50_us`, `wall_clock_append_p99_us` — over all
  Append calls in the cell.
- `wall_clock_checkpoint_put_ms` — single Put when the trigger fires.
- `wall_clock_thaw_ms` — single thaw on warm-start cells.
- `disk_bytes_*` — `std::filesystem::file_size` after the cell.
- `kv_bytes_per_1024_tokens` — recorded per (model_class, kv_dtype)
  cell for Chart C.

Cross-architecture thaw is opportunistic: if the cell's
`architecture_tag` differs from the producer's, the policy returns
`must_refill_from_log = true` and the cell records that as data, not
failure.

## Phase 3 — adversarial scenarios

The four scenarios in `configs/adversarial.yaml` each correspond to a
specific tamper site:

| Scenario | Tamper site | Detection mechanism |
|---|---|---|
| `manifest_hash_mismatch_architecture_tag` | `producer.architecture_tag` byte | `ComputeManifestHash` recomputation |
| `manifest_hash_mismatch_artifact_hash` | `model.artifact_hash` byte | `ComputeManifestHash` recomputation |
| `cross_tenant_inject` | identity at Put time | `CheckpointStore::Put` content-address mismatch |
| `malformed_kv_payload` | mid-record byte | length-prefix invariant in `DecodeCheckpointPayload` |

Each scenario emits one row with `tamper_test_json` populated; the
chart D renderer reads that field and colors red when
`thaw_decision != "ok"`.

## Phase 4 — rig matrix

Three rigs run the same configs:

- R1: x86_64 + XNNPack CPU, fp16
- R2: arm64 + Hexagon NPU, int8 (policy-gated, opt-in)
- R3: arm64 + Apple Neural Engine, fp16

Each rig writes its baselines under
`baselines/<rig>-<config_name>.jsonl`. The plot pipeline takes the
union of files; the cross-rig fallback story emerges from the data.

## Phase 5 — writeup

`docs/benchmarks/2026-04-projection-cliff.md` consumes the SVGs and
adds the per-chart paragraph. Skeptic-resistant order is enforced by
the section ordering (E first, A last).

## Open questions

- Where does the "single TCP MTU" reference line on Chart C land
  visually given the y-axis log scale? Adjust if it becomes a fixed
  sliver near the bottom.
- Should mock rows be allowed in production JSONL inputs, or should
  `plot.py` reject them by default? Current code emits a warning but
  proceeds; that may be the wrong default for the writeup pipeline.
- Is the `summarization_baseline` condition wired via the upstream
  conversation manager, or does it need its own driver path?
