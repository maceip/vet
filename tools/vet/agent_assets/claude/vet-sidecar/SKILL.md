---
name: vet-sidecar
description: Use VET as a DPM sidecar for project memory, corrections, and stale-fact suppression during Claude Code work.
---

# VET Sidecar

Use VET when a task depends on previous project context, benchmark decisions,
handoffs, or user corrections.

At the start of the task, run:

```sh
${VET_BIN:-vet} handoff --task "<current task>"
```

Treat correction events as authoritative over older conflicting facts. Do not
reuse invalidated facts in plans, answers, code comments, PR text, or handoffs.
Invalidated facts can appear inside correction metadata so you know what to
suppress; those quoted facts are not active memory.

Record important durable context:

```sh
${VET_BIN:-vet} record --type user --payload "<important user constraint>"
${VET_BIN:-vet} record --type model --payload "<accepted decision or result>"
${VET_BIN:-vet} record --type tool --payload "<important tool result>"
```

When the user corrects the project story or bench interpretation:

```sh
${VET_BIN:-vet} correction \
  --text "<what changed>" \
  --invalidated-fact "<old wrong fact>" \
  --replacement-fact "<new fact>"
```

For dense sessions, run `${VET_BIN:-vet} prompt --task "<current task>"` and
use the returned DPM projection prompt to make a compact, cited memory view
before proceeding.

For scripted validation, use a fresh VET root/session and an explicit Claude
model/budget. VET logs are append-only, so interrupted smoke tests should not
reuse a release or benchmark session.
