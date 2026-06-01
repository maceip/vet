---
name: vet-sidecar
description: Use VET as durable project memory with corrections and verifiable handoffs for Claude Code.
---

# VET sidecar

Use VET when a task depends on earlier project decisions, benchmark state, handoffs, or user corrections.

## Start of task

```sh
${VET_BIN:-vet} handoff --task "<current task>"
```

## Verifiable JSON handoff

When you need to check that memory was not tampered with:

```sh
${VET_BIN:-vet} handoff --task "<current task>" --format json --out .vet/handoff.json
${VET_BIN:-vet} verify --bundle .vet/handoff.json --json
```

Run `verify` before trusting a handoff from the host. If verification fails, read `failure_details` in the JSON output.

`vet init` creates `.vet/<tenant>/<session>/aid.json`, an **Agent Identity Document (AID)** for the session.

Verification checks the log fingerprint and corrections. It does **not** prove language model (LLM) or tool HTTP calls happened.

## Record durable context

```sh
${VET_BIN:-vet} record --type user --payload "<important user constraint>"
${VET_BIN:-vet} record --type model --payload "<accepted decision or result>"
${VET_BIN:-vet} record --type tool --payload "<important tool result>"
```

## Record corrections

When the user fixes a stale fact:

```sh
${VET_BIN:-vet} correction \
  --text "<what changed>" \
  --invalidated-fact "<old wrong fact>" \
  --replacement-fact "<new fact>"
```

Corrections supersede older conflicting facts. Do not reuse invalidated facts in plans, answers, comments, pull requests, or future handoffs.

## Long sessions

```sh
${VET_BIN:-vet} prompt --task "<current task>"
```

Use the returned projection prompt to build compact, cited memory before acting.

For smoke tests, use a fresh VET root and session so test records do not pollute real project memory.
