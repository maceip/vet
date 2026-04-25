# Baselines

Per-rig JSONL outputs from the projection-cliff benchmark. Each file is
one driver invocation; concatenating files across rigs is the input that
`plot.py` consumes for the chart pipeline.

Naming convention:

```
<rig>-<config>.jsonl
```

Examples:

- `r1-x86-trajectory-sweep.jsonl`
- `r2-arm64-hexagon-costs.jsonl`
- `r3-arm64-ane-architecture.jsonl`

Files prefixed `mock-` are produced by `--mock` runs of `plot.py` and
should not be checked in alongside production results; they exist only
for the layout-review path.

The chart pipeline tolerates concatenating files from different rigs;
the `architecture_tag` field is the partition key in the resulting
plots so cross-architecture rows are visible side-by-side.
