# Phase 1 Status

> **Note:** This file is an internal engineering checklist, not the user guide.
> For how to use VET today, start at [`README.md`](../../README.md) and
> [`docs/architecture-overview.md`](../../docs/architecture-overview.md).

> **Phase 1 is complete for stateless replay and paper-aligned DPM runtime
> semantics. Remaining work is performance tuning for long-sequence prefill
> and Phase 2 checkpointing, not Phase 1 correctness.**

This is the reviewer-facing closure checklist. It maps every Phase 1 item
from `.codex-local/_2_structure.md` (and the audit follow-ups it spawned)
into one of four buckets so reviewers can score Phase 1 in a single pass.
For deeper context on individual items see `PHASE1_AUDIT_RECOVERY.md`.

## Bucket A — Complete in code

Append-only mmap-backed event log substrate
- `runtime/platform/eventlog/PosixEventSink` — magic header, length-prefixed
  records, inter-process file lock + process-local path mutex,
  fsync/FlushFileBuffers, mmap reads, cheap `ProbeGeneration` cache key.

Paper-aligned DPM core
- `Event` struct + `kCorrection` enum.
- `EventSourcedLog` decoded-event facade over injectable `EventSink`.
- `DPMProjector` + `ProjectionConfig` (`memory_budget_chars`, `schema_id`,
  `schema_json`, `model_id`, `seed`, `temperature`).
- `StatelessDecisionEngine` end-to-end (append → project → decide → append).
- Schema-anchored projection prompt with three-section ordering, verbatim
  numeric anchors, one-based `[i]` citations, MEMORY BUDGET banner.
- Decider prompt.
- JSON validation on projection output; citation enforcement rejects `[0]`.

Statelessness guarantees
- `EngineDPMInferenceRunner` rejects `fresh_context=false`, requires a pinned
  `model_id`, asserts `GetCurrentStep() == 0` (or `kUnimplemented`).
- Executor `Reset()` in all three executors clears KV buffers, rolls back to
  step 0, invalidates pending tokens.
- `SessionConfig::SetForceKvResetBeforePrefill(true)` propagates from the DPM
  runner into the serial and threaded execution managers, which call
  `llm_executor->Reset()` before every `Tasks::Prefill`. Closes the literal
  Predict-loop KV reset requirement from `_2_structure.md`.

Replay / audit
- Replay-safe timestamps (request + response timestamps required; wall-clock
  capture is opt-in).
- Pinned `model_id` stamped on every `kModel` event.
- Multi-tenant isolation by construction (path namespacing + per-record
  identity validation on read).
- Cross-tenant appends rejected.

Configuration
- `runtime/proto/dpm_config.proto` is the source of truth.
- `runtime/dpm/config/dpm_config_loader` parses proto-text and adapts into
  `DPMLogIdentity`, `DPMProjector::ProjectionConfig`,
  `StatelessDecisionEngine::Config`, and `EventSink::RetentionPolicy`.

Retention sidecar
- `EventSink::RetentionPolicy` and `AppendRecordWithRetention`. Sinks that
  do not support retention return `Unimplemented` rather than dropping it.
- `PosixEventSink` writes a JSON sidecar at
  `events.dpmlog.retention.json`. Bucket-level Object Lock on the underlying
  S3 bucket remains the load-bearing immutability mechanism (verified
  empirically; see `PHASE1_AUDIT_RECOVERY.md`).

Determinism test
- `EventSourcedLog → projection prompt` is byte-identical across 10 replays.

## Bucket B — Accepted architectural substitution

| `_2_structure.md` item | Substitution | Verified by |
|---|---|---|
| AWS SDK for C++ multipart uploader thread | S3 Files NFS mount + `PosixEventSink` writing the canonical log | April 25, 2026 empirical probe (Object Lock applies, `fdatasync` is durable, concurrent O_APPEND is atomic, Lambda mounts work) |
| S3 Object Lock metadata API plumbing in the runtime | Bucket-level Object Lock at provisioning + optional retention sidecar for per-session overrides | Empirical probe + sidecar unit tests |
| YAML config | proto-text `DpmConfig` (matches the rest of LiteRT-LM's config story) | Loader unit tests |

Each substitution preserves the property the original item delivered while
keeping the runtime free of a new external SDK.

## Bucket C — Deferred perf work (tracked, does not block Phase 1)

- XNNPack / ML Drift long-sequence parallel prefill tuning + benchmark
  harness. The opt-in benchmark entry point is
  `//tools/benchmarks/dpm_prefill_bench`, with baseline schema at
  `tools/benchmarks/baselines/dpm_prefill_baselines.json`.
- Pinned-real-model byte-identical determinism test as opt-in CI. Test
  scaffold lives at `runtime/dpm/dpm_determinism_e2e_test.cc` and runs only
  when `LITERTLM_ENABLE_E2E_DETERMINISM_TEST=1` and
  `DPM_DETERMINISM_MODEL_PATH` points at a pinned model artifact.

## Bucket D — Not Phase 1 runtime code

- Phase 2: hierarchical checkpoints (Merkle SHA-256 NEON kernel, gRPC
  streaming, Amazon MemoryDB hydration). Tracked in `_2_structure.md`
  Phase 2.
- Phase 3: audit synthesis (`ShadowAuditor`, Lambda → SQS FIFO → EC2 G6e
  drift detection, `kCorrection` interrupt path). Tracked in
  `_2_structure.md` Phase 3.

## Reviewer checklist

When evaluating Phase 1 closure:

1. Open this file. Score each item under Bucket A as ✅ / ❌.
2. Confirm Bucket B substitutions still hold for your deployment shape (in
   particular: do you intend to mount an S3 Files file system with
   bucket-level Object Lock at provisioning?).
3. Confirm Bucket C items remain acceptable as deferred perf work for the
   first deployment milestone.
4. Bucket D is for posterity; nothing from Phase 2/3 is in scope here.

If every item under Bucket A is ✅ and Bucket B is acceptable, Phase 1 is
done. Bucket C and Bucket D do not block.
