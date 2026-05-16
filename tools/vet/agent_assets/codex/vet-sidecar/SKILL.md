---
name: vet-sidecar
description: Use the VET DPM sidecar to preserve task memory, apply corrections, and avoid carrying stale facts across coding-agent turns.
---

# VET Sidecar

Use this skill when the task depends on prior project decisions, benchmark state,
corrections, or a handoff from another coding agent.

Resolve the executable as `${VET_BIN:-vet}`. At the start of the task, run:

```sh
${VET_BIN:-vet} handoff --task "<current task>"
```

Use the handoff as task memory. Correction events supersede older conflicting
facts; invalidated facts must not appear in the active plan, answer, or future
handoff.

Record durable events when they will help a later agent:

```sh
${VET_BIN:-vet} record --type user --payload "<important user constraint>"
${VET_BIN:-vet} record --type model --payload "<accepted decision or result>"
${VET_BIN:-vet} record --type tool --payload "<important tool result>"
```

When the user corrects prior project memory, record the correction directly:

```sh
${VET_BIN:-vet} correction \
  --text "<what changed>" \
  --invalidated-fact "<old wrong fact>" \
  --replacement-fact "<new fact>"
```

For long or ambiguous sessions, ask VET for the projection prompt and use it to
produce a compact task memory before acting:

```sh
${VET_BIN:-vet} prompt --task "<current task>"
```
