# VET Sidecar Release Assets

VET is a small DPM sidecar binary for coding agents. It gives Claude Code,
Codex, Gemini CLI, and similar tools one shared way to record working memory,
record corrections, and request a task-conditioned handoff that suppresses stale
facts.

Build the binary:

```sh
bazelisk build //tools/vet:vet
```

Use it from a repo:

```sh
./bazel-bin/tools/vet/vet init
./bazel-bin/tools/vet/vet record --type user --payload "We are stabilizing the Phase 3 bench story."
./bazel-bin/tools/vet/vet correction \
  --text "The stale escape metric means final-answer escape only." \
  --invalidated-fact "stale_memory_escape counts any stale text in memory" \
  --replacement-fact "stale escape is only counted when stale facts reach the final answer"
./bazel-bin/tools/vet/vet handoff --task "Continue the benchmark release notes"
```

The event log is stored under `.vet/<tenant>/<session>/events.dpmlog` by
default. That log is append-only; VET does not rewrite history. Forgetting is
handled by correction events and correction-aware projection prompts, so agents
carry forward the right active memory without hiding the audit trail.

Agent assets:

- `agent_assets/codex/vet-sidecar/SKILL.md`: Codex skill instructions.
- `agent_assets/claude/vet-sidecar/SKILL.md`: Claude Code skill instructions.
- `agent_assets/gemini/vet-sidecar/`: Gemini CLI extension source.

Install an asset:

```sh
tools/vet/install_agent_asset.sh codex --scope project
tools/vet/install_agent_asset.sh claude --scope project
tools/vet/install_agent_asset.sh gemini
```

The assets assume `vet` is on `PATH`. During local development, set
`VET_BIN=/absolute/path/to/bazel-bin/tools/vet/vet` if the binary is not
installed globally.

## Claude Code Smoke Test

For non-interactive Claude CLI validation, use a fresh temp project, a fresh VET
session, and an explicit low-cost model. This keeps append-only smoke records
out of real project memory and avoids model auto-selection changing cost.

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

In the handoff output, invalidated facts may be quoted inside
`[VET BLOCKING CORRECTIONS]` or `[RECENT EVENT LOG - AUDIT ONLY]`; that is
expected. They are suppression targets or audit content, not facts the agent
should carry forward.
