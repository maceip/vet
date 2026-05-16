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
