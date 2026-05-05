---
title: Append-only agent memory
description: A substrate where the event log never changes and the memory the agent acts on is recomputed from it on demand.
---

# Append-only agent memory

> Today's agents rewrite their memory every few turns. By hour two of a real session the agent is acting on a copy of a copy of a copy of what you actually said. We measured a 492-event session where this approach took 17 model calls to produce a memory that had lost the user's original instruction. We built an alternative — append-only event log, memory recomputed at decision time, every projection cryptographically tied to the events that produced it.

## Why this happens

Every popular agent framework today has the same memory architecture: a running summary string that gets rewritten every N turns. The "memory" at any given moment is the output of the latest summarize call, which took the previous summary as input and produced a new one. That summary will become the input to the next summarize call. And the next.

Which means: the user's first instruction has been re-summarized, re-summarized, and re-summarized again before the agent makes any decision more than a few turns in. The agent isn't acting on what the user said. It's acting on a generation-loss reproduction of what the user said.

The failure mode this produces is recognizable. The agent forgets the original ask. The agent fixates on the most recent thing it touched. The agent confidently asserts a fact that isn't in the source events but appears in the latest summary. None of this is a model-quality bug. It's a substrate bug.

## What an alternative looks like

Three primitives:

**An append-only event log.** Every user turn, every tool call, every result is appended. Nothing is rewritten. The log is the source of truth for the entire session — and unlike a rolling summary, the log is the *full* truth, not a derivation of it.

**A projection at decision time.** When the agent needs to act, it doesn't read a stored memory. It runs a single task-conditioned projection over the event log: a function from `(events, task) → memory`, executed once, used once, discarded. The memory the agent acts on is recomputed every time, never edited.

**A content-addressed audit certificate.** Every projection produces a checkpoint. Replaying the raw log against the projection model produces bytes that either hash-match the stored projection (verdict: pass) or don't (verdict: blocking correction emitted). The certificate is a child node of the checkpoint in a Merkle DAG. If the projection drifts from what the events actually say, the runtime gate refuses the next decision until a fresh projection is taken.

The first two are the recomputable-memory substrate. The third makes the memory *provably faithful* — every decision is bound to the specific events that produced it, not to a summary nobody can audit.

## What we measured

A 492-event Claude session, compressed to a 1338-character memory two ways:

- Rolling-summary: 17 model calls, 8 035 output tokens, original user instruction lost in the final memory.
- Append-only with a single task-conditioned projection: 2 model calls, 750 output tokens, original instruction preserved.

**8.5× fewer calls, 10.7× fewer output tokens, better answer quality.**

Two AgenticQwen rubric-shaped twin pairs (one synthetic seed, one real row from `alibaba-pai/AgenticQwen-Data`). On the policy-allowed twin, rolling-summary's compressed memory dropped the policy-allowed tool *names* entirely — the agent could no longer recommend them when asked what to do next. The projection-based memory preserved them. Synthetic seed: 0/3 vs 3/3. Real data: 0/3 vs 1/3. The pattern holds across both; the magnitude is data-dependent.

A 17-event session with auditor-rubric content embedded in prior agent turns. After hardening the projection prompt against instruction injection from event content, rolling-summary's memory had drifted entirely off the original ask (0/8 keywords retained from the user's first instruction). The projection-based memory led with the original ask (4/8).

The benchmark methodology and the schema-locked JSONL are in the repo. The score schema rejects rows whose `bytes_scored_from` doesn't match their `test_kind` at construction time, and the chart code refuses to render rows the schema didn't accept — so a fake compression-quality comparison is structurally impossible to plot, not just unlikely.

## Honest limits

Faithful compression cuts both ways. On the policy-violating twin of one real-data case, the projection-based memory faithfully preserved the user's request *including the forbidden operations* the user was pushing for. The agent under that memory proposed two policy-violating tool calls that the rolling-summary agent did not. Faithful compression preserves whatever is in the events; without an explicit policy constraint in the prompt, that includes the user's pressure tactic.

Long sessions beyond a single projection call. A 6 335-event case exceeds the input budget of one model call by ~6×. Hierarchical projection at the substrate layer — the same `Level0` + `DeltaAppend` codec that already exists in the C++ runtime — is what addresses this; the bench has not yet run it.

## What's next

The audit substrate above — replay verifier, content-addressed certificates, fail-closed runtime gate, blocking corrections — is real code with green tests today. The bench has not yet exercised the audit pipeline end-to-end against real session data; that's the follow-on post.

The full machinery is in the [LiteRT-DPM repo](https://github.com/maceip/LiteRT-DPM). Substrate index: [`runtime/dpm/PHASE2_STATUS.md`](https://github.com/maceip/LiteRT-DPM/blob/phase2-substrate/runtime/dpm/PHASE2_STATUS.md). Replay auditor entry point: [`runtime/dpm/projection_replay_auditor.h`](https://github.com/maceip/LiteRT-DPM/blob/phase3-substrate/runtime/dpm/projection_replay_auditor.h) at commit `51b0bcb8`. Bench JSONL + schema lock: [`tools/benchmarks/dpm_projection_cliff/runs/`](https://github.com/maceip/LiteRT-DPM/tree/phase2-bench/tools/benchmarks/dpm_projection_cliff/runs).

---

*The project name internally is **DPM** — Deterministic Projection Memory. The page calls it "append-only agent memory" because that's the property worth caring about; the rest is implementation detail.*
