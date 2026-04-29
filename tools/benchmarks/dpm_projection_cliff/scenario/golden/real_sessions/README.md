# Curated golden SessionCases — five-shape coverage

Five real-session SessionCase JSONs, one per shape category, drawn from
the LiteRT-DPM contributors' local Claude Code and Codex session logs.
Curated by `tools/benchmarks/dpm_projection_cliff/scenario/curate_real_sessions.py`,
**pre-redacted at write time**, and committed verbatim. The substrate
property tests on `phase2-substrate` consume these from a stable
checked-in path; they MUST NOT depend on live `~/.claude` /
`~/.codex` access.

## What's checked in

| File | Bucket | Notes |
|---|---|---|
| `short.session_case.json` | smallest viable session | Lower bound on event count for the property tests. |
| `long.session_case.json` | largest event count seen in the population | Upper bound; stresses range-coverage and rollup. |
| `correction_heavy.session_case.json` | most user-correction signals | Drives the correction-detection scenario; substrate must retain multiple corrections without flattening. |
| `tool_heavy.session_case.json` | highest tool_call density | Drives next-tool-call scenario; substrate must surface tool sequences. |
| `handoff_ish.session_case.json` | session that survived a context compaction | Closest analogue we have to a real cross-session handoff in the wild. |
| `index.json` | metadata | Per-bucket counts, generation timestamp, curator script reference. NO raw paths. |

## What is and isn't in these JSONs

**Is:**
- Normalized events from session-start to a probe-point T, in the same
  shape `ingest_session.py` produces from any session.
- Auto-derived ground-truth probes (`next_user_intent`,
  `next_tool_call`, `correction_detection`).
- A `source_sha256` so we can ID the original session if needed for
  curation refresh, without revealing where it lived.

**Isn't:**
- Raw session log paths. `source_path` is replaced with a
  `<curated:domain:sha8>` placeholder by the curator before write.
- Credentials of any kind. Every event passed through the deterministic
  redactor (`scenario/redactor.py`); credentials matching any of its
  patterns are replaced with length-stable placeholders.
- Probe rubrics. These are free-form session captures — the
  rubric-shaped fields (`must_call_tools`, etc.) are present in the
  schema but empty by default. Rubric-bearing cases come from the
  AgenticQwen lane (`agentic_qwen/`), not here.

## Refresh

If the source population changes (new sessions accumulate, an old
session is retired) and the golden set should be regenerated:

```bash
python tools/benchmarks/dpm_projection_cliff/scenario/curate_real_sessions.py
git diff tools/benchmarks/dpm_projection_cliff/scenario/golden/real_sessions/
```

Selection is deterministic given the same scored population, so
re-running on an unchanged corpus produces a no-op diff.

## What this set does NOT prove

These cases give the substrate property tests real-shape input, but
they are not by themselves a benchmark. They prove that:

- `LoadSessionCasesFromFile` round-trips real session shapes.
- The substrate primitives (manifest, hash, codec, MerkleDagStore)
  operate correctly on real-event-derived inputs.

They do NOT prove "DPM beats rolling-summary on real sessions." That
requires the differential scoring layer that's still missing — same
unsolved gap called out in `agentic_qwen/DRIFT_REPORT_2026-04-29.md`.
