# DPM Projection Cliff Benchmark

The benchmark designed to convince a skeptical audience that DPM's
architectural promise survives contact with real hardware.

## What this benchmark answers

Five questions, in the order a skeptical engineer asks them:

1. **What does this cost?** Disk per session, memory overhead per active
   session, wall-clock for Append and Put, build-time impact, lines of
   code added.
2. **Where does the win come from?** Decompose the cliff into the four
   architectural decisions (single-call projection, transport
   quantization where opted in, warm-thaw checkpoints, prefix-cached
   projection prompts) and quantify each independently.
3. **Did you pick architectures or trick the codec?** KV-cache size by
   model class (dense / GQA / MQA / MLA / sliding-window) under the
   replay-safe codec (fp16) vs the policy-gated lossy codec (int8).
4. **Does the integrity claim actually fire on tampering?** Inject a
   tampered manifest into a synthetic DAG; show
   `EvaluateCheckpointThawVerification` lights it red and names the
   field that differs.
5. **And does the cliff hold?** Trajectory-length sweep with the four
   conditions side-by-side; decision-alignment score on the left axis,
   wall-clock latency on the right axis (log scale).

## Skeptic-resistant chart order

The writeup leads with costs and ends with the cliff:

1. **Chart E — Costs first.** Honest measurement of every cost added.
2. **Chart B — Mechanism breakdown.** Each architectural decision
   contributing a measurable percentage to the win.
3. **Chart C — Checkpoint size by architecture.** Bytes per 1024 tokens,
   colored by replay-safety, with a horizontal "single TCP MTU" reference
   line so the size comparison is grounded.
4. **Chart D — Adversarial audit.** A DAG with one tampered node; the
   audit query lights it red and names the mismatch field.
5. **Chart A — The projection cliff.** The headline payoff chart, only
   after the skeptic has had four off-ramps that each closed.

## Repository layout

```
tools/benchmarks/dpm_projection_cliff/
├── README.md                  (this file)
├── BUILD                      (Bazel target)
├── CMakeLists.txt             (CMake target)
├── dpm_projection_cliff.cc    (driver binary)
├── plot.py                    (matplotlib chart generator)
├── configs/
│   ├── trajectory_sweep.yaml  (the headline sweep)
│   ├── costs.yaml             (cost-only sweep, fast)
│   ├── architecture.yaml      (model-class × dtype matrix)
│   └── adversarial.yaml       (tampered-DAG scenario)
└── baselines/
    └── README.md              (where rig-specific JSONL outputs land)
```

## Reproduction

The driver binary emits one JSONL row per (condition, trajectory_length,
repeat) tuple. Each row carries enough metadata for byte-exact replay:
manifest_hash of the model artifact, BLAKE3 of the input config,
runtime version, the rig's architecture_tag.

```sh
# Run a single config on the local rig.
bazel run --enable_workspace //tools/benchmarks/dpm_projection_cliff \
    -- --config=tools/benchmarks/dpm_projection_cliff/configs/costs.yaml \
       --output_jsonl=tools/benchmarks/dpm_projection_cliff/baselines/local-costs.jsonl

# Render the charts from one or more JSONL files.
python tools/benchmarks/dpm_projection_cliff/plot.py \
    --input tools/benchmarks/dpm_projection_cliff/baselines/*.jsonl \
    --output_dir docs/benchmarks/2026-04-projection-cliff/
```

The plot script emits one SVG per chart (E, B, C, D, A) with deterministic
file names so they can be embedded in markdown docs and slide decks
without manual regeneration.

## Test rigs

Three rigs exercise the cross-architecture story explicitly. Each rig
records its `architecture_tag` in the JSONL so the cross-rig fallback path
is visible in the data.

| Rig | ISA | Accelerator | Default KV dtype |
|---|---|---|---|
| R1  | x86_64 | CPU + XNNPack | fp16 |
| R2  | arm64  | Hexagon NPU   | int8 (policy-gated) |
| R3  | arm64  | Apple Neural Engine | fp16 |

A row in the output JSONL whose `manifest_hash` was produced on R1 but
read on R2 will fail `EvaluateCheckpointCompatibility` with reason
`architecture_tag mismatch` and trigger `must_refill_from_log`. That
data point is logged, not retried — the cross-architecture fallback is
the story we want to surface.

## Limitations (listed up front)

- Cross-tenant isolation is a correctness property, not a perf property;
  not measured here.
- AWS providers (S3 Express, MemoryDB, EKS gRPC, RDMA Tier-0) are
  Phase 2.2 partner work; the benchmark runs locally and on
  EC2-without-MemoryDB.
- The cliff at very long trajectories (>1M characters) is extrapolation;
  hierarchical projection is the documented escape hatch and is out of
  scope here.
- We pin `manifest_hash` per model bundle; model-version drift across a
  chain is intentionally not exercised.

## Mock data while the driver is being wired

Until the driver is fully integrated against the merged
`runtime/dpm/` + `runtime/platform/` substrate, `plot.py` accepts a
`--mock` flag that synthesizes plausible JSONL with clearly-labeled
mock rows so the chart shapes and writeup can be reviewed without
hardware. Mock rows carry `"mock": true`; the production driver will
never set that field.
