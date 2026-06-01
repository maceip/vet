![vet](./vet.webp)

# VET — memory you can check for coding agents

**VET** is a small command-line program that sits beside coding agents like Claude Code, Codex, and Gemini CLI. It keeps a **durable project memory** that agents can replay, correct, and hand off to the next session.

Most agents rely on a rolling chat summary. Summaries are easy to use, but they can quietly drop old facts or keep facts that are no longer true. VET adds an **append-only event log** (a file that only grows; nothing is erased) plus tools to **record corrections** and **verify handoffs** before you trust them.

## Who this is for

Use VET when an agent must remember things like:

- release rules or benchmark decisions
- user corrections ("that old fact was wrong")
- tool results that should survive a session restart or agent swap

## Quickstart

### 1. Get the binary

Download a release for your platform from [GitHub Releases](https://github.com/maceip/vet/releases):

- macOS (Apple Silicon): `VET-macos-arm64`
- Windows (64-bit): `VET-windows-x86_64.exe`

Put it on your path as `vet`:

```sh
chmod +x VET-macos-arm64
sudo mv VET-macos-arm64 /usr/local/bin/vet
```

Windows PowerShell:

```powershell
New-Item -ItemType Directory -Force "$env:USERPROFILE\bin" | Out-Null
Move-Item .\VET-windows-x86_64.exe "$env:USERPROFILE\bin\vet.exe"
```

### 2. Start a session in your repo

```sh
vet init
```

This creates a local folder `.vet/` (ignored by git) with:

- `events.dpmlog` — the append-only memory log
- `aid.json` — an **Agent Identity Document (AID)** that describes this session

### 3. Record facts and corrections

```sh
vet record --type user --payload "Release builds must ship macOS and Windows binaries."
vet record --type model --payload "CI uploaded vet-v0.1.0 with checksum files."

vet correction \
  --text "The escape metric definition changed." \
  --invalidated-fact "escape counts any stale text in memory" \
  --replacement-fact "escape counts only when stale text reaches the final answer"
```

A **correction** does not delete old log entries. It adds a new entry that tells the agent which facts to stop using.

### 4. Hand memory to an agent

Plain-text handoff (easy to paste into a chat):

```sh
vet handoff --task "Continue the release notes"
```

JSON handoff (for verification):

```sh
vet handoff --task "Continue the release notes" --format json --out handoff.json
vet verify --bundle handoff.json --json
```

If verification passes, you see `"verified": true`. If someone tampered with the log or bundle, verification fails with a named reason in `failure_details`.

## How hybrid mode works

VET does **not** replace your coding agent.

The agent still uses its normal chat history for flow. VET adds a **replayable memory layer** for facts that must survive handoffs and corrections:

1. **`vet record`** — append important events to the log
2. **`vet correction`** — mark old facts as invalid and add replacements
3. **`vet handoff`** — produce task memory that respects corrections
4. **`vet prompt`** — build a compact, cited memory view from the log
5. **`vet verify`** — check that a JSON handoff still matches the live log

**Forgetting happens by correction**, not by erasing history. The log stays complete for audit; handoffs tell the agent what is *active* vs *suppressed*.

## VeriHandoff — the worked example

**VeriHandoff** is the end-to-end demo that shows verification working. It includes:

| Piece | What it does |
|-------|----------------|
| `run_demo.sh` | Runs a full session, verifies, then shows tampering fails |
| `verify.html` | Browser page to read verify results without parsing JSON |
| Golden fixtures + CI | Automated check that verification still passes |

```sh
bazelisk build --config=vet_release_no_android //tools/vet:vet
tools/vet/examples/verihandoff/run_demo.sh
open tools/vet/examples/verihandoff/verify.html
```

Details: [`tools/vet/examples/verihandoff/README.md`](./tools/vet/examples/verihandoff/README.md)

### What verification checks

- session identity matches
- Agent Identity Document (AID) unchanged
- log digest matches (BLAKE3 hash over the log file)
- event counts and correction counts match
- log structure is valid (order, identities)

### What verification does **not** check

- whether the host agent ran the right tools
- whether a language model (LLM) API call really happened
- raw HTTP transcripts from tools

Those limits are listed in `aid.json` under `claims.does_not_verify`.

## Agent setup

Ready-to-copy skill files live in [`tools/vet/agent_assets`](./tools/vet/agent_assets):

```sh
tools/vet/install_agent_asset.sh codex --scope project
tools/vet/install_agent_asset.sh claude --scope project
tools/vet/install_agent_asset.sh gemini
```

Each skill tells the agent to call `vet handoff` before relying on old memory and `vet correction` when the user fixes a stale fact.

If `vet` is not on your path:

```sh
export VET_BIN=/absolute/path/to/vet
```

## Common commands

```sh
vet status --json
vet events --max-events 20
vet record --type tool --payload "bazel build //tools/vet:vet succeeded"
vet handoff --task "Review the release workflow"
vet prompt --task "Summarize decisions for the next agent"
```

**Event types**

| Type | Use for |
|------|---------|
| `user` | durable user constraints or decisions |
| `model` | accepted agent decisions or outcomes |
| `tool` | important command results |
| `internal` | local notes that should be replayable |
| `correction` | invalidations and replacement facts |

## Storage

Default location: `.vet/<tenant>/<session>/`

Custom location:

```sh
vet init --root /secure/vet/logs --tenant acme --session release
vet handoff --root /secure/vet/logs --tenant acme --session release --task "Ship release"
```

## Build from source

```sh
bazelisk build --config=vet_release_no_android //tools/vet:vet
```

The `vet_release_no_android` build flag avoids pulling Android build rules on machines that happen to have Android tools installed.

## Learn more

- Architecture (Deterministic Projection Memory runtime): [`docs/architecture-overview.md`](./docs/architecture-overview.md)
- VET tool reference: [`tools/vet/README.md`](./tools/vet/README.md)
- Agent gate hooks demo: [`tools/agent_hooks/README.md`](./tools/agent_hooks/README.md)
- Benchmarks: [`tools/benchmarks/`](./tools/benchmarks/)
- Public explainer site: https://maceip.github.io/vet/

## Related research

This project shares a name with the academic paper **VET Your Agent: Towards Host-Independent Autonomy via Verifiable Execution Traces** (Oxford, December 2025):

https://arxiv.org/abs/2512.15892

That paper focuses on cryptographic proof that an autonomous agent's outputs match a declared configuration, even when the host machine is untrusted. This repository's **VET sidecar** applies a related idea to coding agents: bind handoffs to an append-only log and an **Agent Identity Document (AID)** so others can run `vet verify` and see whether memory was tampered with. We do not implement the paper's web proofs or trading-agent demo; see [`tools/vet/examples/verihandoff/`](./tools/vet/examples/verihandoff/) for our worked example.
