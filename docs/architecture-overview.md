![vet](../vet.webp)

# Architecture overview

This page explains how **VET** (the coding-agent memory sidecar) fits together with the **Deterministic Projection Memory (DPM)** runtime underneath it.

**Read the public explainer:** https://maceip.github.io/vet/

## The problem in plain terms

Long-running coding agents need memory. Rolling chat summaries are convenient, but they can:

- drop important facts
- keep facts that the user already corrected
- hand off work with no way to check what changed

VET keeps a **complete, append-only log** and rebuilds **task memory** from that log when it matters.

## Big ideas

### Append-only event log

Every user message, tool result, model note, and correction is stored as an **event** in `events.dpmlog`. New events are appended; old events are not edited away.

### Task-conditioned projection

**Projection** means: read the log, pick what matters for the current task, and format it as memory the agent can use. The DPM runtime builds projection prompts deterministically from the log so the same log + task yields the same prompt.

### Corrections are first-class

When the user says an old fact is wrong, VET records a **correction event**. Handoffs and projection prompts treat corrected facts as **suppressed**, not as active memory.

### Hybrid mode

Agents keep their normal rolling chat for fluency. VET adds a **checked memory layer** at important boundaries:

- before tool use (via optional agent hooks)
- at handoff between agents or sessions
- when resuming work after a correction

### Verifiable handoffs (VET sidecar)

The `vet` binary can emit a **JSON handoff bundle** tied to:

- an **Agent Identity Document (AID)** in `aid.json` — session metadata and what verification claims to check
- a **trace digest** — a BLAKE3 hash fingerprint of the log file
- correction metadata copied from the log

Anyone with the live `.vet/` folder can run:

```sh
vet verify --bundle handoff.json --json
```

The **VeriHandoff** demo (`tools/vet/examples/verihandoff/`) shows this end to end, including a browser verifier UI and continuous integration (CI) checks on golden fixtures.

Verification checks log integrity and session binding. It does **not** prove that a language model (LLM) API call occurred or that the host agent followed every rule.

## Runtime layout

| Area | Role |
|------|------|
| [`runtime/dpm/`](../runtime/dpm/) | Event log, projection prompts, projector, stateless decision engine |
| [`runtime/platform/`](../runtime/platform/) | Checkpoint manifests, Merkle provenance, audit certificates |
| [`tools/vet/`](../tools/vet/) | Sidecar CLI: `init`, `record`, `correction`, `handoff`, `verify` |
| [`tools/agent_hooks/`](../tools/agent_hooks/) | Demo hooks that gate tool use using correction-aware context |

## Agent hooks (optional demo layer)

Hooks run a small Python gate before tool use. Supported surfaces:

| Agent | Config location |
|-------|-----------------|
| Claude Code | `.claude/settings.json` |
| Codex CLI | `.codex/config.toml` |
| Gemini CLI | `.gemini/settings.json` |
| Cursor Agent | `.cursor/hooks.json` |
| GitHub Copilot | `.github/hooks/dpm-gate.json` |

Smoke test:

```sh
python tools/agent_hooks/dpm_gate.py --agent claude --reset
python tools/agent_hooks/dpm_gate.py --agent claude --demo-seed
python tools/agent_hooks/dpm_gate.py --agent claude --status
```

## Benchmarks

Benchmark scripts live under [`tools/benchmarks/`](../tools/benchmarks/). They compare memory strategies such as:

- full chat history (high context cost, no audit trail)
- rolling summary (common baseline, weak provenance)
- audited projection (stricter, replayable)
- rolling summary plus VET gate (practical adoption path)

## Build note

This repository follows the upstream [LiteRT-LM](https://github.com/google-ai-edge/LiteRT-LM) layout. For focused VET work:

```sh
bazelisk build --config=vet_release_no_android //tools/vet:vet
bazelisk test --config=vet_release_no_android //tools/vet:vet_stage2_test
```

On Windows, if Android environment variables are set, clear them before broad builds unless you intentionally need Android targets.

## Credits

Built on Google's [LiteRT-LM](https://github.com/google-ai-edge/LiteRT-LM).

The Deterministic Projection Memory (DPM) research direction is described in Srinivasan et al., [arXiv:2604.20158](https://arxiv.org/abs/2604.20158).

The verifiable handoff shape in `vet` is inspired by the separate academic **Verifiable Execution Traces (VET)** paper (Oxford, [arXiv:2512.15892](https://arxiv.org/abs/2512.15892)). This repo's sidecar implements log-bound verification for coding agents; it does not implement that paper's cryptographic web proofs or trading-agent demo.
