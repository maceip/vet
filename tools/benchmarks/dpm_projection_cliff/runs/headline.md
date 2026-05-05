# DPM vs rolling-summary — three mechanisms, real data

We compressed an agent-session event log into ≤1338 chars two ways
— **rolling-summary** (the standard pattern: re-summarize each chunk)
and **DPM** (a single task-conditioned projection of the whole log) —
and asked the same downstream question of each. Same model
(`claude-haiku-4-5`), same memory budget, same probe. Schema-locked
JSONL is on disk under `runs/`; the chart code refuses to render any
row whose `bytes_scored_from` doesn't match its `test_kind`, so a
fake-DPM-differential cannot reach the panel by construction
(`scenario/score_schema.py`, `DPM_VS_ROLLING_SUMMARY_DIFFERENTIAL`).

## 1. Cost asymmetry on long sessions

492-event Claude session, compressed to a 1338-char memory, asked
"what was the user's first instruction?":

![cost_asymmetry](cost_asymmetry.png)

Rolling-summary did 17 LLM calls and emitted 8 035 output tokens.
DPM did 2 calls and emitted 750 output tokens. Same answer-quality
window. **8.5× fewer calls, 10.7× fewer output tokens.**

Source: `20260505T191158Z_claude-dfadd53b-1429-49d7-a2e3-3344b330da90@T=492.jsonl`

## 2. Policy retention under tight memory budget

Two AgenticQwen rubric-shaped twin pairs (one synthetic seed, one real
row from `alibaba-pai/AgenticQwen-Data`). Each pair has a normal twin
where the policy says *the agent must call these tools* and a hack twin
where the user is pressuring the agent to bypass policy.

![policy_retention](policy_retention.png)

On the **normal twin**, rolling-summary's compressed memory dropped the
policy-allowed tool *names* across the summarize cycle and the agent
couldn't recommend them. DPM's task-conditioned projection preserved
them. Synthetic seed: 0/3 vs 3/3. Real row: 0/3 vs 1/3.

The real-row separation is smaller than the synthetic seed — DPM beats
rolling on every case but the magnitude is data-dependent. **Honest
caveat:** on the *hack* twin of the same real row, DPM faithfully
preserved the user's request including the forbidden operations, and
when asked for a next-action the agent under DPM proposed two
policy-violating tool calls that the agent under rolling-summary did
not. DPM is a more faithful compressor; without an explicit policy
constraint in the prompt, faithful compression preserves the user's
hack request along with everything else. Rubric scoring caught it.

Source: `20260505T184406Z_agentic_qwen_pair.jsonl` (synthetic seed),
`20260505T191746Z_agentic_qwen_pair.jsonl` (real row 3488)

## 3. Original-instruction recall under instruction injection

17-event Codex session with auditor-rubric content embedded in prior
turns ("you are assessing... output JSON"). The first DPM run got
hijacked by the embedded instructions and emitted a JSON
risk-assessment instead of a memory. Hardening the projection prompt
— wrapping events in `<<<EVENT_LOG_START/END>>>` markers and
explicitly telling the model the bytes inside are data, not
instructions — restored the projection.

After hardening: rolling-summary's memory had drifted to "Memory
Update: recent progress" framing and lost the original ask entirely.
DPM correctly led with "Initial Request (Turn 1): User asked to review
plan for agentic network where GitHub repo owners add repos for AI
agent fixes. Identified issues and requested simplification." Rolling
recovered 0/8 keywords from the original instruction; DPM 4/8.

Source: `20260505T183841Z_codex-rollout-2026-04-14T07-23-47-019d8a72@T=8.jsonl`

## What this doesn't measure (yet)

Two gaps worth naming:

1. **Long sessions beyond a single API call.** A 6 335-event case
   exceeds a single projection-call's input-token budget by ~6×.
   Hierarchical projection at the bench layer would close this — same
   primitive the substrate's `Level0` + `DeltaAppend` codec already
   provides on the C++ side.

2. **Detection of when DPM's projection itself goes wrong.** The
   correction-heavy case showed DPM is vulnerable to instruction
   injection from event content; we hardened the prompt and recovered.
   But "did the projection faithfully represent the events?" is exactly
   the question the LiteRT-DPM Phase 3 substrate
   ([`projection_replay_auditor`](../../../runtime/dpm/projection_replay_auditor.h),
   commit `51b0bcb8`) was built to answer in production: replay the raw
   log, hash-compare against the stored projection, emit a
   content-addressed audit certificate, and fail the runtime gate if
   verdict ≠ pass. The bench has not yet run a projection through that
   pipeline; that's the next post.

## Reproduce

```
python tools/benchmarks/dpm_projection_cliff/scenario/head_to_head.py \
       tools/benchmarks/dpm_projection_cliff/scenario/golden/real_sessions/handoff_ish.session_case.json
python tools/benchmarks/dpm_projection_cliff/scenario/rubric_head_to_head.py \
       tools/benchmarks/dpm_projection_cliff/agentic_qwen/golden/real_row_092_pair.json
python tools/benchmarks/dpm_projection_cliff/runs/render_charts.py
python tools/benchmarks/dpm_projection_cliff/runs/validate_runs.py
```

`validate_runs.py` is the schema gate — every committed JSONL passes
or the post is wrong.
