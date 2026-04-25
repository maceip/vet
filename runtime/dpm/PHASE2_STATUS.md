# Phase 2 Status

> **Phase 2 substrate is complete: the data formats, the durable stores, the
> Merkle DAG, and the policy decisions are in code. Remaining work is the
> cloud / hardware providers (S3 Express, MemoryDB, RDMA, NPU mmap thaw)
> tracked as Phase 2.2.**

This is the reviewer-facing closure checklist. Score Phase 2 in a single
pass by walking the four buckets below.

## Bucket A — Complete in code

### Checkpoint ABI (`runtime/proto/checkpoint.proto`)
- `CheckpointAbi` strict header: identity / level / parent_hashes (vec, DAG-
  not-chain) / producer / model / kv_dtype / base_event_index / body_hash /
  body_size_bytes / created_unix_micros / **manifest_hash** (covers every
  header field; tampering with any one breaks the digest).
- `CheckpointStorageBinding` enforcing metadata-tier and blob-tier rules at
  the type level.
- `CheckpointTrigger` enum: handoff / milestone-tool / token-threshold /
  idle-speculative / compaction / manual. **Entropy-shift omitted by
  construction.**
- `CheckpointTransportTier`: local mmap / RDMA-RoCE / gRPC + FlatBuffers.
- `KvDtype`: fp16 / int8-per-token / int4-channel.
- `ModelClass`: dense / GQA / MQA / MLA / sliding-window.

### Hash substrate (`runtime/platform/hash/`)
- `Hasher` interface + `Hash256` POD.
- `Sha256Hasher` (pure C++, FIPS 180-4 reference, pinned to NIST CAVS).
- `Blake3Hasher` (pure C++, BLAKE3 spec, pinned to length-0/1/1023/1024/
  1025/8192 reference vectors).
- `CreateHasher(algo)` + `HashBytes(algo, data)`.
- No SDK / BoringSSL / OpenSSL dependency.

### Canonical manifest (`runtime/platform/checkpoint/canonical_manifest.{h,cc}`)
- Versioned binary encoding (`kCanonicalManifestVersion=1`) over every ABI
  header field.
- `ComputeManifestHash(algo, input)` → Hash256, the Merkle DAG node identity.

### KV transport quantization (`runtime/platform/checkpoint/kv_quantization.{h,cc}`)
- `kFp16`: bit-exact round-trip; the **default and replay-safe** codec.
- `kInt8PerToken`: per-(token, head) absmax scaling; **opt-in only** under
  a `KvDtypePolicy` that has been audited via thaw-equivalence test.
- `kInt4Channel`: reserved.
- `KvDtypePolicy` + `PickReplaySafeKvDtype` + `RequireReplaySafeKvDtype`
  policy gates.

### Hierarchical payload codec (`runtime/platform/checkpoint/checkpoint_codec.{h,cc}`)
- `Level0` full snapshots and `DeltaAppend` tail deltas.
- `EncodeCheckpointPayload` / `DecodeCheckpointPayload` round trip.
- `ReconstructFromChain` replays Level0 + N deltas (compaction reset
  honored mid-chain).

### Content-addressed checkpoint store (`runtime/platform/checkpoint/`)
- `CheckpointStore` interface (Put / Get / Exists / List).
- `LocalFilesystemCheckpointStore` reference backend.
- Durable writes through `DurablyWriteFile` (atomic temp + fsync + rename +
  dir-fsync).
- Idempotent Put with content verification — same address with different
  bytes returns DataLoss instead of silently keeping the old file.

### Merkle DAG store + provenance (`runtime/platform/provenance/`)
- `MerkleDagStore` interface + `LocalMerkleDagStore` durable backend.
- Multi-parent nodes (DAG, not chain) with diamond-merge support.
- `GetCheckpointProvenance(store, leaf)` → deterministic BFS topological
  ordering.

### Append-only event log + CoW branching (`runtime/platform/eventlog/`)
- `PosixEventSink::CreateBranch` substrate operation. O(1) on-disk write
  via durable branch-pointer sidecar; O(parent records) validation;
  O(visible records) read with depth-bound recursion.
- Branch and retention sidecars now go through `DurablyWriteFile`.

### Policy layer (`runtime/dpm/checkpoint_policy.{h,cc}`)
- `EvaluateCheckpointCompatibility(manifest, request)` — strict identity /
  model / artifact-hash / architecture / kv-dtype-policy match. Soft falls
  to `must_refill_from_log` on any mismatch.
- `EvaluateCheckpointThawVerification(expected, actual)` — manifest_hash
  mismatch → re-prefill.
- `ShouldCreateCheckpoint(policy, state)` — handoff > correction >
  max-token > context-pressure > milestone-tool > idle-speculative.
- `SelectCheckpointTransportTier(request)` — rack-local + RDMA → RDMA;
  else gRPC.
- `ShouldCompactCheckpointDeltas(chain, policy)` — defaults 8 levels /
  128 MiB.
- `ValidateCheckpointStorageTiers(binding)` — type-level enforcement of
  metadata = MemoryDB, blob ≠ MemoryDB.

### Two-phase upload (`runtime/platform/checkpoint/upload_session.{h,cc}`)
- `CheckpointUploadSession` Begin → Add (ordered, contiguous) → Finalize.
- Finalize commits via `CheckpointStore::Put`, returning the content-
  addressed Hash256.
- Crash-safe: temp file on rename, lost in-memory buffer just restarts
  the upload.

### Prefix-cached projection (`runtime/dpm/projection_prompt.{h,cc}`)
- `ProjectionPromptParts { cacheable_prefix, event_log_suffix, Compose() }`
  surface. Same config + different event log → identical prefix bytes.

## Bucket B — Accepted architectural substitution

| Item | Substitution | Verified by |
|---|---|---|
| `BranchLogRef` struct describing a branch | `PosixEventSink::CreateBranch` runtime operation with parent-pointer sidecar | Tests exercise read concatenation, depth bound, and durability |
| Hand-rolled JSON for digest determinism | Versioned binary `canonical_manifest` encoding for the DAG hash. Canonical JSON is still available at the dpm boundary for cross-language (e.g. Lambda) consumers but is not the digest. | Cross-language interop is a Phase 2.2 deliverable; binary hash works today |
| `ManifestDigestFn` injection | Concrete `Hasher` from `runtime/platform/hash/` | Round-trip tests + reference vectors |
| `parent_checkpoint_id` single-string chain | `parent_hashes: vec<Hash256>` DAG | Diamond-merge provenance test |

## Bucket C — Deferred perf work (tracked, does not block Phase 2)

- BLAKE3 SIMD batching and multi-threaded parallel mode — single-threaded
  reference shipped; partner can swap when the Merkle hot path needs it.
- KV codec for `kInt4Channel` — reserved enum, codec returns Unimplemented.
- Speculative checkpointing scheduler — trigger decision is in code; the
  scheduler that fires it during user-idle time is partner work.

## Bucket D — Not Phase 2 runtime code (Phase 2.2 partner)

- `S3ExpressCheckpointStore` (drops in behind `CheckpointStore` interface).
- MemoryDB metadata adapter for `CheckpointStorageBinding`.
- EKS gRPC ingest service + RDMA / RoCE Tier 0 transport.
- Vendor-specific NPU mmap thaw path (Hexagon / Apple Neural Engine /
  ML Drift).
- LMDB or mmap'd ring-buffer outbox replacing the SQLite "Pending" buffer
  from the Phase 1 plan.
- Real KV-tensor extract / import on LiteRT backends.

## Reviewer checklist

When evaluating Phase 2 closure:

1. Score each item under Bucket A as ✅ / ❌.
2. Confirm the Bucket B substitutions still work for the deployment shape
   (in particular: the manifest_hash is the DAG identity; binary canonical
   encoding is the digest; canonical JSON is for cross-language readers
   only).
3. Confirm Bucket C items remain acceptable as deferred perf work.
4. Bucket D is for posterity; Phase 2.2 is partner territory.

If every item under Bucket A is ✅ and Bucket B is acceptable, Phase 2 is
done. Bucket C and Bucket D do not block.
