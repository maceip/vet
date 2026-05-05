# Bench runs

Schema-validated `ScoreRow` JSONL files emitted by the head-to-head
runner and other differential runners.

Each `*.jsonl` file is one run. Each line is one `ScoreRow` validated
against `scenario/score_schema.py`. Charts only consume rows that pass
`validate_runs.py` — there is no path from raw stdout into a chart.

## Layout

```
runs/
  <ISO8601>_<case_id>.jsonl     # one run per file, committed when headline
  validate_runs.py              # CI guard: every committed JSONL must validate
  README.md                     # this file
  .gitignore                    # excludes raw stdout / tmp files
```

## Adding a run to the public record

1. Run `head_to_head.py` (or the equivalent runner) — it writes the
   JSONL to `runs/` automatically.
2. Sanity-check with `python runs/validate_runs.py runs/<file>.jsonl`.
3. `git add -f runs/<file>.jsonl` (the `.gitignore` does NOT block
   `*.jsonl`, but be explicit — only commit runs you intend to publish).
4. Commit with a message that names which case + which substrate set
   the run covers.

## Phase 3 forward-compatibility

The schema's `compression_substrate` enum already includes
`checkpointed_dpm`, so once Phase 3's audit-gated checkpoint substrate
is in place, the same runner shape (with one additional substrate row
per case) keeps the JSONL schema stable. Charts that already filter on
`DPM_VS_ROLLING_SUMMARY_DIFFERENTIAL` will accept the new rows
unchanged.
