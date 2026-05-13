# AgenticQwen-Data adapter — secondary validation

> **Read this first.** This corpus is **secondary validation, not the
> Phase 2 main proof.** AgenticQwen rows fit in one context window
> (~7–15K tokens each); they cannot exercise hierarchical projection,
> rollup, replay-from-raw, or checkpoint DAG integrity. They DO test
> whether DPM's task-conditioned projection preserves
> policy-critical facts and tool-order constraints under tight memory
> budgets — which is the projection-quality claim, distinct from the
> beyond-context-window claim.

## What this directory is

An adapter that turns rows from `alibaba-pai/AgenticQwen-Data`
([HuggingFace](https://huggingface.co/datasets/alibaba-pai/AgenticQwen-Data),
[arXiv:2604.21590](https://arxiv.org/abs/2604.21590)) into the same
`SessionCase` shape the substrate scenario tests already consume.

The dataset's distinctive value: it carries **explicit machine-checkable
rubrics** — `must_call_tools`, `must_not_call_tools`,
`database_state_must_remain` — that our free-form session captures
don't have. That lets us tighten scenario probes from substring match
to rubric-shaped ground truth.

## Output shape

Each AgenticQwen row produces **two `SessionCase` records** linked by
`paired_case_id`:

| Case role  | What its rubric carries |
|---|---|
| `normal` | `must_call_tools` derived from `tool_return_expected_json.normal_path` — the policy-compliant sequence |
| `hack`   | `must_not_call_tools` derived from `tool_return_expected_json.hack_path` — the policy-violating sequence the agent must reject |

Both twins share the same events prefix (the conversation up to
`probe_T`); only their probe rubric differs. This is the **differential
test** scaffolding: scenario tests run an assertion on both paths and
verify the substrate's projection preserves the same policy
constraints regardless of path.

## What's checked-in

| File | Role |
|---|---|
| `ingest_agentic_qwen.py` | Adapter: AgenticQwen row → `SessionCase[]`. Accepts parquet (via pyarrow), JSON, or JSONL inputs. |
| `golden/synthetic_seed_row.json` | One AgenticQwen-shaped row (no real PII) covering a Winnipeg utility-relocation policy violation. Lets the adapter be tested without downloading the real corpus. |
| `golden/synthetic_seed_pair.json` | Adapter output for the seed row above. The C++ scenario test's twin-pair fixture. Regenerate with: `python ingest_agentic_qwen.py golden/synthetic_seed_row.json --out golden/synthetic_seed_pair.json` |

## How to use the real dataset

```bash
# One-time download (~80 MB):
huggingface-cli download alibaba-pai/AgenticQwen-Data --repo-type dataset \
    --local-dir /tmp/agentic_qwen_data

# Adapt the first 200 rows into our SessionCase corpus:
python tools/benchmarks/dpm_projection_cliff/agentic_qwen/ingest_agentic_qwen.py \
    /tmp/agentic_qwen_data/data/train-00000-of-00001.parquet \
    --limit 200 \
    --out tools/benchmarks/dpm_projection_cliff/agentic_qwen/cases_first_200.json
```

Output is the same JSON the substrate's scenario tests already load.

## What probes the rubric drives

The adapter emits one probe per twin, of kind `policy_preserved`:

```json
{
  "kind": "policy_preserved",
  "question": "Under tight memory pressure, did the projected memory ...",
  "rationale": "agentic_qwen:<normal|hack>_path",
  "must_include":          [...],   // policy facts the projection MUST surface
  "must_not_include":      [...],   // facts the projection MUST NOT surface
  "must_call_tools":       [...],   // (normal) tools the agent MUST invoke next
  "must_not_call_tools":   [...],   // (hack)   tools the agent MUST NOT invoke
  "database_state_must_remain": [...],
  "judge_rubric": "<test_policy text>"
}
```

The C++ scenario test (`phase2_scenario_test.cc`) reads these via the
typed `ProbeRubric` struct and runs assertions appropriate to each
twin role.

## Where this fits in the matrix

`PHASE2_TEST_MATRIX.md` (on `phase2-substrate`) defines the primary
matrix. AgenticQwen lives **outside** that matrix — it's an additional
validation lane labeled **secondary**. The headline chart it
contributes to is:

> *Under tight memory pressure, which memory substrate preserves the
> policy blocker / tool precondition?*

DPM, rolling-summary, raw-context, and (eventually) checkpointed DPM
are scored on the same probe; the rubric tells us not just "did the
agent get the next tool right" but "did the substrate retain enough of
the policy that the agent COULD have gotten it right."

## What this lane explicitly does NOT prove

- Cross-context resumption (rows fit in one context).
- Hierarchical / rollup correctness (no nesting in the rows).
- Long-horizon handoff (no shift boundaries).
- Real-user signal (the dataset is synthetic-by-design).

For those, the primary substrate matrix and the real agent-session corpus
under `scenario/` remain the load-bearing tests.
