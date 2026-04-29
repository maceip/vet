# AgenticQwen-Data adapter — drift report

Run date: 2026-04-29.
Source: `alibaba-pai/AgenticQwen-Data` from HuggingFace, single-file
`agenticqwen_synthetic_data.parquet` (331 MB, 37,401 rows).
Adapter: `tools/benchmarks/dpm_projection_cliff/agentic_qwen/ingest_agentic_qwen.py`.
Sample: first 100 rows -> 200 SessionCases (one normal + one hack twin per row).

## Schema verification

The adapter's assumed top-level columns matched the parquet schema
exactly:

```
id, system, user, task_background, rubrics, test_policy,
user_escape_strategy, messages_json, tool_return_expected_json
```

All nine fields are `string`. `messages_json` and
`tool_return_expected_json` are JSON-encoded strings, not native
nested types.

## Drift bugs found and fixed during this run

### Bug 1: tool field name (FIXED)

The original adapter looked for `step.get("tool")` or
`step.get("name")` inside `tool_return_expected_json`. The real dataset
uses **`tool_name`**. Fixed by adding `tool_name` to the lookup,
falling through to `tool` / `name` for compatibility with synthetic
fixtures.

Pre-fix coverage of `must_call_tools`: **0 / 200 cases (0%)**.
Post-fix coverage: **99 / 200 cases (50%)**.

### Bug 2: rubrics shape (FIXED)

The original adapter `splitlines()`'d the rubrics text, expecting a
multi-line list. The real dataset writes rubrics as a single inline-
numbered string:

```
Completion is determined by: 1) The assistant must query the resident
record first. 2) Upon discovering the address mismatch and past move-in
date, it must NOT call remove_old_utility_registration or
register_new_utility_account. 3) The database state must remain
unchanged: ... 4) The response must explain that ...
```

Fixed by splitting on numbered-clause separators (`\d+\)`) before
applying keyword heuristics. Coverage of state-invariant detection
went from 0% (line-split missed everything) to **86 / 200 cases (43%)**.

### Bug 3: hack twin contained legitimate tools (FIXED)

The original adapter populated `must_not_call_tools` with EVERY tool
in `hack_path`. Many of those tools (e.g. `query_resident_record`)
appear in BOTH normal and hack paths -- they are legitimate first
steps; the hack only diverges later. Marking them as must-not-call
contradicted the normal twin's `must_call_tools` and would have
produced spurious test failures.

Fixed by computing `must_not_call = hack_set - normal_set` (the
policy-violating subset only).

Verification: **0 / 100 twin pairs have overlap** between
normal.must_call and hack.must_not_call after the fix. Pre-fix this
overlap would have been ~100% on cases where both paths share a
legitimate first step.

## Coverage of rubric fields after fixes

```
                            populated/total   median size when populated
must_include                40/200  (20%)     1
must_call_tools             99/200  (50%)     2
must_not_call_tools         68/200  (34%)     1
database_state_must_remain  86/200  (43%)     1
```

## Sparse-coverage diagnosis (where the remaining gap comes from)

About half the cases have machine-checkable tool sequences. The
remaining half fall into three patterns the adapter does not yet
capture:

1. **Cases where normal and hack invoke the same tools but with
   different inputs.** Example: both paths call
   `update_resident_address` but normal passes the verified new
   address while hack passes the unverified one. Tool-name-only
   diffing puts these in neither must_call nor must_not_call. Future
   work: surface the input-arg constraint as a separate rubric field
   (`must_call_with_input` / `must_not_call_with_input`).
2. **Cases where the rubric phrases the policy as facts not actions.**
   Example: "the assistant must inform the user that retroactive
   updates are not supported." No tool to call or not call; the
   constraint is on the agent's natural-language response. The
   adapter's `must_include` only catches a subset of these because
   the keyword set ("must explain", "must include", "must contain",
   "must reference", "response must") doesn't cover every phrasing.
   Future work: an LLM-judged variant.
3. **Cases where the rubric uses synonyms the adapter doesn't match.**
   "do not invoke", "should never", "is not permitted to call" all
   map semantically to must-not-call but the adapter's keyword set
   doesn't recognize them. Tractable to extend; left as future work.

## What this means for the bench

- The adapter is **real** -- it produces non-empty rubric fields on
  half-or-more of cases at the dataset's actual scale. It is not
  fixture-only.
- 50% rubric coverage is **enough signal** to use AgenticQwen as a
  secondary validation lane. Cases with empty rubric fields are still
  loadable and can be scored via the legacy substring-style probes;
  they just don't contribute to the strict tool-sequence assertion.
- Three documented future-work items (input-arg constraints,
  judge-scored facts, synonym extension) would push coverage above
  80% without changing the adapter's semantic shape.

## What still has to happen before AgenticQwen scores anything

The adapter produces SessionCases. The substrate's scenario tests
parse them. **Neither one yet runs a projection through DPM and
compares its output bytes against the rubric.** That scoring layer
is the load-bearing missing piece. The C++ scenario test currently
asserts loader-level invariants (twin pairs share events, rubrics
load) -- not "DPM beats rolling-summary on policy retention."

The next concrete step is to wire the rubric into a real differential
benchmark: project the events under DPM and under rolling-summary at
the same memory budget, then score the projection bytes against
must_call_tools / must_not_call_tools / database_state_must_remain
via substring presence + the test_policy LLM-judged rubric.
