# VET sidecar tool

The **VET** command-line program gives coding agents a shared way to:

- record durable project memory
- record user corrections
- produce task handoffs that suppress stale facts
- verify JSON handoffs against the live log

Build:

```sh
bazelisk build --config=vet_release_no_android //tools/vet:vet
```

Binary path after build: `bazel-bin/tools/vet/vet`

## Basic workflow

```sh
./bazel-bin/tools/vet/vet init
./bazel-bin/tools/vet/vet record --type user --payload "We freeze the release harness unless a serious bug appears."
./bazel-bin/tools/vet/vet correction \
  --text "The escape metric definition changed." \
  --invalidated-fact "escape counts any stale text in memory" \
  --replacement-fact "escape counts only when stale text reaches the final answer"
./bazel-bin/tools/vet/vet handoff --task "Continue the release notes"
```

## Verifiable JSON handoffs

```sh
./bazel-bin/tools/vet/vet init
./bazel-bin/tools/vet/vet handoff --task "Continue release notes" --format json --out handoff.json
./bazel-bin/tools/vet/vet verify --bundle handoff.json --json
```

`vet init` writes `.vet/<tenant>/<session>/aid.json`, an **Agent Identity Document (AID)** that describes the session.

The JSON handoff binds the task to:

- AID digest
- BLAKE3 trace digest over `events.dpmlog`
- event range and correction metadata

On failure, `failure_details` names the check and explains why it failed.

## VeriHandoff case study

VeriHandoff is the complete worked example (command-line demo, browser verifier, golden fixtures, CI). See [`examples/verihandoff/README.md`](./examples/verihandoff/README.md).

```sh
chmod +x tools/vet/examples/verihandoff/run_demo.sh
tools/vet/examples/verihandoff/run_demo.sh
open tools/vet/examples/verihandoff/verify.html
tools/vet/examples/verihandoff/verify_golden.sh
```

## What lives on disk

Default log path: `.vet/<tenant>/<session>/events.dpmlog`

The log is **append-only**. VET does not rewrite history. Corrections add new events; handoffs and projection prompts tell agents which older facts to suppress.

## Agent skill files

| Agent | Path |
|-------|------|
| Codex | `agent_assets/codex/vet-sidecar/SKILL.md` |
| Claude Code | `agent_assets/claude/vet-sidecar/SKILL.md` |
| Gemini CLI | `agent_assets/gemini/vet-sidecar/` |

Install:

```sh
tools/vet/install_agent_asset.sh codex --scope project
tools/vet/install_agent_asset.sh claude --scope project
tools/vet/install_agent_asset.sh gemini
```

Skills assume `vet` is on `PATH`. During development:

```sh
export VET_BIN=/absolute/path/to/bazel-bin/tools/vet/vet
```

## Claude Code smoke test

Use a fresh temp project and session so smoke records do not pollute real memory:

```sh
tmp="$(mktemp -d)"
mkdir -p "$tmp/.claude/skills/vet-sidecar" "$tmp/bin"
cp tools/vet/agent_assets/claude/vet-sidecar/SKILL.md \
  "$tmp/.claude/skills/vet-sidecar/SKILL.md"
ln -sf "$(pwd)/bazel-bin/tools/vet/vet" "$tmp/bin/vet"

(
  cd "$tmp"
  PATH="$tmp/bin:$PATH" claude -p \
    --model haiku \
    --no-session-persistence \
    --max-budget-usd 0.25 \
    --tools Bash \
    --allowedTools 'Bash(vet *)' \
    'Use /vet-sidecar. With the vet CLI only, initialize session claude-smoke, record one user fact, record one correction, run status, run handoff, and return compact JSON confirming the replacement fact is active and the invalidated fact is only a suppression target.'
)
```

In handoff output, invalidated facts may appear inside `[VET BLOCKING CORRECTIONS]` or `[RECENT EVENT LOG - AUDIT ONLY]`. That is expected: they are suppression targets or audit text, not facts to reuse.
