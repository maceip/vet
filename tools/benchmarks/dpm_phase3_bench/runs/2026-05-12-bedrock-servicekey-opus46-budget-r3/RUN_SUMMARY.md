# Phase 3 Scatter Sweep: Bedrock Opus 4.6

Run ID: `bedrock-servicekey-opus46-budget-r3`

Model path: AWS Bedrock service key, `eu.anthropic.claude-opus-4-6-v1` with `global.anthropic.claude-opus-4-6-v1` backfill.

Artifacts:

- `results.scored.jsonl`: 150 scored rows.
- `results.merged.jsonl`: 152 rows including 2 quota-error rows.
- `scatter_points.csv`: 50 aggregate scatter points.
- `scatter_aggregates.csv`: condition x budget summary table.
- `missing_cells.csv`: 12 cells not filled before Bedrock daily token quota.
- `scatter_quality_vs_calls.svg`: quality vs model calls.
- `scatter_quality_vs_tokens.svg`: quality vs token load.
- `scatter_quality_vs_stale_escape.svg`: quality vs stale-memory escape.

Caveat: Bedrock daily token quota stopped the long-real-session tail. The dataset is plot-ready, but not a fully complete 162-row matrix.
