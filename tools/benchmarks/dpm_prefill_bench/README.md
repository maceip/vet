# DPM Prefill Benchmark

`dpm_prefill_bench` measures the Phase 1 hot path after projection prompt
construction: a fresh session prefilling a DPM-shaped prompt over an
approximately 27k-character synthetic event log.

The benchmark is a follow-up workstream for XNNPack and ML Drift tuning. It
does not gate Phase 1 correctness.

The public benchmark story and chart assessment live in
[`docs/benchmarks/dpm-red-team-benchmark.html`](../../../docs/benchmarks/dpm-red-team-benchmark.html).
That page is intentionally wired to show a pending-baseline state until
hardware-owned result JSON is checked in.

Example:

```sh
bazel run --enable_workspace //tools/benchmarks/dpm_prefill_bench \
  -- --model_path=/path/to/model.litertlm \
     --backend=cpu \
     --iterations=3 \
     --num_cpu_threads=4 \
     --output_json=tools/benchmarks/baselines/dpm_prefill_cpu_local.json
```

Run the same target with `--backend=npu` on hardware that has the ML Drift path
available. The checked-in baseline file is intentionally a schema placeholder
until hardware-owned numbers are captured.
