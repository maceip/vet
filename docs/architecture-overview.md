![vet](../vet.webp)

# vet: agent memory audit system

> Long-running agents need memory, but memory gets edited. vet keeps the useful parts of rolling memory while adding a replayable audit layer that can prove where a decision came from, apply corrections, and block stale actions before they reach tools.

**Read the explainer:** https://maceip.github.io/vet/

**Bench data:** [`tools/benchmarks/`](../tools/benchmarks/)

vet is a C++ runtime substrate and agent-hook demo for auditable agent memory. It is built on Google's [LiteRT-LM](https://github.com/google-ai-edge/LiteRT-LM) and extends deterministic projection memory into a practical hybrid mode.

## Genesis

The original DPM insight is simple: do not ask an agent to remember by endlessly rewriting its own memory. Record what happened, project only what matters for the next decision, and verify that projection before the agent acts.

vet keeps that shape:

- **Append-only log** - every user message, tool call, result, and correction is recorded.
- **Task-conditioned projection** - decision memory is rebuilt from the log at action boundaries.
- **Audit certificate** - the rebuilt memory is tied to an event range, manifest hash, model identity, and gate verdict.

In hybrid mode, those pieces wrap the agent's existing rolling memory. Rolling memory keeps the session fluent; vet checks the state the agent is about to use when mistakes would escape into tools, handoff, or resumed work.

The goal is not to replace every memory system. The goal is to make existing agents safer at the moments that matter: tool use, handoff, resuming old work, and acting after the user corrected a prior assumption.

## What It Does

- **Hybrid memory gate** - lets rolling memory keep the agent fluent, while vet verifies action-boundary memory against an append-only log.
- **Correction-aware replay** - records user corrections as first-class events and prevents invalidated facts from leaking into later actions.
- **Decision receipts** - every accepted projection has a manifest hash, audit certificate, event range, model identity, and gate verdict.
- **Replayable handoff** - another agent or future session can resume from a checked memory artifact instead of an unverifiable summary.
- **Local-first hooks** - demo adapters let common coding agents call the same gate before tool use.

## Supported Agents

The hook adapters live in [`tools/agent_hooks/`](../tools/agent_hooks/).

| Agent | Hook surface |
|---|---|
| Claude Code | `.claude/settings.json` |
| Codex CLI | `.codex/config.toml` plus `tools/agent_hooks/codex_user_hook.ps1` |
| Gemini CLI | `.gemini/settings.json` |
| Cursor Agent | `.cursor/hooks.json` plus `tools/agent_hooks/cursor_agent_gate.py` fallback wrapper |
| GitHub Copilot | `.github/hooks/dpm-gate.json` |

Smoke the local gate:

```powershell
python tools/agent_hooks/dpm_gate.py --agent claude --reset
python tools/agent_hooks/dpm_gate.py --agent claude --demo-seed
python tools/agent_hooks/dpm_gate.py --agent claude --status
```

Synthetic denial example:

```powershell
'{"hook_event_name":"PreToolUse","tool_name":"Write","tool_input":{"content":"transport as the main result"}}' |
  python tools/agent_hooks/dpm_gate.py --agent claude
```

## Core Runtime

The runtime lives under [`runtime/dpm/`](../runtime/dpm/) and [`runtime/platform/`](../runtime/platform/).

The main pieces are:

- append-only event logging
- deterministic projection prompts
- checkpoint manifests and Merkle provenance
- audit certificates
- correction payloads and correction barriers
- a fail-closed checkpoint loader for decisions

## Benchmarks

Benchmarks live under [`tools/benchmarks/`](../tools/benchmarks/).

The most useful current framing is hybrid:

- full chat history: highest context cost, no memory audit
- rolling summary: common baseline, good continuity, weak provenance
- audited projection: cheaper and replayable, but stricter
- rolling summary plus vet gate: adoption path for existing agents

## Build

This repo follows the upstream LiteRT-LM build shape. For local work on this Windows machine, avoid accidentally invoking Android builds unless that is the task:

```powershell
$env:ANDROID_NDK_HOME=""
$env:ANDROID_NDK_ROOT=""
$env:BAZEL_SH="C:\Program Files\Git\bin\bash.exe"
```

Run focused tests before broad builds. The Phase 3 bench Python smoke tests are:

```powershell
python tools\benchmarks\dpm_phase3_bench\bench_schema.py
python tools\benchmarks\dpm_phase3_bench\score.py
```

## Credits

Built on Google's [LiteRT-LM](https://github.com/google-ai-edge/LiteRT-LM). The original deterministic projection memory architecture is from Srinivasan et al., [arXiv:2604.20158](https://arxiv.org/abs/2604.20158).
