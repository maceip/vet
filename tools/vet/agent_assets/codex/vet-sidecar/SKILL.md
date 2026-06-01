---
name: vet-sidecar
description: Use the VET sidecar for durable project memory, corrections, and verifiable handoffs in Codex.
---

# VET sidecar

Use this skill when the task depends on prior project decisions, benchmark state, corrections, or a handoff from another coding agent.

Resolve the executable as `${VET_BIN:-vet}`.

## Start of task

```sh
${VET_BIN:-vet} handoff --task "<current task>"
```

## Verifiable JSON handoff

```sh
${VET_BIN:-vet} handoff --task "<current task>" --format json --out .vet/handoff.json
${VET_BIN:-vet} verify --bundle .vet/handoff.json --json
```

Run `verify` before trusting host-supplied memory. On failure, read `failure_details`.

Verification checks log integrity and corrections. It does not prove language model (LLM) or tool HTTP calls.

## Record durable events

```sh
${VET_BIN:-vet} record --type user --payload "<important user constraint>"
${VET_BIN:-vet} record --type model --payload "<accepted decision or result>"
${VET_BIN:-vet} record --type tool --payload "<important tool result>"
```

## Record corrections

```sh
${VET_BIN:-vet} correction \
  --text "<what changed>" \
  --invalidated-fact "<old wrong fact>" \
  --replacement-fact "<new fact>"
```

Correction events supersede older conflicting facts.

## Dense sessions

```sh
${VET_BIN:-vet} prompt --task "<current task>"
```

Use the projection prompt to build compact task memory before acting.
