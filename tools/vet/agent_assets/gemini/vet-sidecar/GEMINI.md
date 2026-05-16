# VET Sidecar

Use VET when a task depends on previous project context, benchmark decisions,
handoffs, or user corrections.

Resolve the executable as `${VET_BIN:-vet}`. At task start, run:

```sh
${VET_BIN:-vet} handoff --task "<current task>"
```

Use the handoff as active task memory. Correction events supersede older
conflicting facts. Invalidated facts must not be reused in plans, answers, code
comments, release notes, or future handoffs.

Record useful durable context:

```sh
${VET_BIN:-vet} record --type user --payload "<important user constraint>"
${VET_BIN:-vet} record --type model --payload "<accepted decision or result>"
${VET_BIN:-vet} record --type tool --payload "<important tool result>"
```

When the user corrects prior memory:

```sh
${VET_BIN:-vet} correction \
  --text "<what changed>" \
  --invalidated-fact "<old wrong fact>" \
  --replacement-fact "<new fact>"
```

For long sessions, run `${VET_BIN:-vet} prompt --task "<current task>"` and use
the returned DPM projection prompt to produce a compact, cited memory view.
