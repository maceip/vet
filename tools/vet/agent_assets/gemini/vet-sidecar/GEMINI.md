# VET sidecar

Use VET when a task depends on previous project context, benchmark decisions, handoffs, or user corrections.

Resolve the executable as `${VET_BIN:-vet}`.

## Start of task

```sh
${VET_BIN:-vet} handoff --task "<current task>"
```

## Verifiable JSON handoff

`vet init` writes an **Agent Identity Document (AID)** to `aid.json`.

```sh
${VET_BIN:-vet} handoff --task "<current task>" --format json --out .vet/handoff.json
${VET_BIN:-vet} verify --bundle .vet/handoff.json --json
```

Run `verify` before trusting a replayed handoff. Verification checks the log fingerprint and corrections, not language model (LLM) or tool HTTP calls.

## Record context

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

Correction events supersede older conflicting facts. Do not reuse invalidated facts in plans, answers, or future handoffs.

## Long sessions

```sh
${VET_BIN:-vet} prompt --task "<current task>"
```

Use the projection prompt for a compact, cited memory view.
