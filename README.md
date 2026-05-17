![vet](./vet.webp)

# VET: memory safety for coding agents

VET is a small sidecar binary for Claude Code, Codex, Gemini CLI, and other coding agents. It gives agents a durable project memory that can be replayed, corrected, and handed off without trusting a stale rolling summary.

Use it when an agent needs to remember project decisions, benchmark state, release constraints, or user corrections across long sessions.

## Quickstart

Download the latest release for your platform:

- macOS arm64: `VET-macos-arm64`
- Windows x86_64: `VET-windows-x86_64.exe`

Release page: https://github.com/maceip/vet/releases

Put the binary on your `PATH` as `vet`:

```sh
chmod +x VET-macos-arm64
sudo mv VET-macos-arm64 /usr/local/bin/vet
```

Windows PowerShell:

```powershell
New-Item -ItemType Directory -Force "$env:USERPROFILE\bin" | Out-Null
Move-Item .\VET-windows-x86_64.exe "$env:USERPROFILE\bin\vet.exe"
```

Initialize memory in a repo:

```sh
vet init
```

Record useful context:

```sh
vet record --type user --payload "The Phase 3 benchmark harness is frozen unless a serious issue is found."
vet record --type model --payload "Release assets must include macOS and Windows VET binaries."
vet record --type tool --payload "CI release vet-v0.1.0 uploaded platform binaries and sha256 files."
```

Record a correction when old memory becomes wrong:

```sh
vet correction \
  --text "The stale escape metric means final-answer escape only." \
  --invalidated-fact "stale_memory_escape counts any stale text in memory" \
  --replacement-fact "stale escape is counted only when stale facts reach the final answer"
```

Give an agent a handoff:

```sh
vet handoff --task "Continue release notes for the VET binary"
```

## Agent Setup

This repository includes ready-to-copy agent assets in [`tools/vet/agent_assets`](./tools/vet/agent_assets). If you build from source, install them with:

```sh
tools/vet/install_agent_asset.sh codex --scope project
tools/vet/install_agent_asset.sh claude --scope project
tools/vet/install_agent_asset.sh gemini
```

The assets all do the same thing: tell the agent to call `vet handoff --task "<current task>"` before relying on prior project memory, and to call `vet correction` when the user fixes a stale or wrong assumption.

If the binary is not on `PATH`, set `VET_BIN`:

```sh
export VET_BIN=/absolute/path/to/vet
```

PowerShell:

```powershell
$env:VET_BIN = "C:\path\to\vet.exe"
```

## How Hybrid Mode Works

VET does not replace your coding agent. It runs beside it.

The agent still uses its normal chat history and rolling memory for flow. VET adds a replayable memory layer for facts that should survive handoffs, restarts, and corrections:

1.  `vet record` appends important events to `.vet/<tenant>/<session>/events.dpmlog`.
2.  `vet correction` records invalidated facts and replacement facts as first-class events.
3.  `vet handoff` prints an agent-readable memory view that includes recent events and blocking corrections.
4.  `vet prompt` emits a deterministic projection prompt for producing compact, cited task memory from the event log.

The important behavior is forgetting by correction. VET keeps the append-only history, but its handoff and projection prompts tell the agent not to carry invalidated facts into plans, tool use, release notes, or later handoffs.

## Common Commands

```sh
vet status --json
vet events --max-events 20
vet record --type tool --payload "bazel build //tools/vet:vet succeeded"
vet handoff --task "Review the release workflow"
vet prompt --task "Summarize project decisions for the next agent"
```

Event types:

- `user` - durable user constraints or decisions
- `model` - accepted agent decisions, summaries, or outcomes
- `tool` - important command results
- `internal` - local notes that should be replayable
- `correction` - stale fact invalidations and replacements

## Storage

By default VET stores local state under `.vet/`, which is gitignored. The event log is append-only; VET does not rewrite history to hide mistakes. Corrections are additional events.

Use a custom root or session when needed:

```sh
vet init --root /secure/vet/logs --tenant acme --session release
vet handoff --root /secure/vet/logs --tenant acme --session release --task "Ship VET"
```

## Build From Source

```sh
bazelisk build --config=vet_release_no_android //tools/vet:vet
```

The `vet_release_no_android` config prevents hosted CI or developer machines with Android NDK variables set from accidentally initializing Android repository rules for this non-Android binary.

## More

- Architecture overview: [`docs/architecture-overview.md`](./docs/architecture-overview.md)
- VET tool docs: [`tools/vet/README.md`](./tools/vet/README.md)
- Benchmarks: [`tools/benchmarks/`](./tools/benchmarks/)
