# Replayable agent memory

> Today's agents rewrite their memory every few turns. By hour two of a real session the agent is acting on a copy of a copy of a copy of what you actually said. We measured a 492-event session where this approach took 17 model calls to produce a memory that had lost the user's original instruction. **No framework on the market today can tell you which events produced the agent's last decision. The memory has been edited too many times to know.**

**👉 [Read the full explainer](https://maceip.github.io/LiteRT-DPM/)** · [DPM Paper](https://arxiv.org/abs/2604.20158) · [Bench data](./tools/benchmarks/dpm_projection_cliff/runs/)

A C++ inference substrate built on Google's [LiteRT-LM](https://github.com/google-ai-edge/LiteRT-LM) implementing **replayable agent memory** — an append-only event log, memory rebuilt from the log at decision time, and every rebuild cryptographically tied to the events that produced it. Includes two extensions: **Hierarchical Checkpoints** for instant agent handoff, and a **replay-audit substrate** that catches drift between the projection and the events that produced it.

---

## The shape of it

Three primitives:

- **An append-only event log.** Every user input, every tool call, every result is appended. Nothing is rewritten.
- **Memory rebuilt at decision time.** A single task-conditioned read over the log, executed once, used once, discarded.
- **A content-addressed audit certificate.** Every rebuild produces a checkpoint; replay verifies it; drift fails the runtime gate closed and emits a blocking correction.

The paper reports 7–15× speedups at tight memory budgets versus summarization-based memory, with equal or better accuracy. This repository ports that architecture into LiteRT-LM's C++ runtime, adds the network and storage primitives for production deployment, and layers the two extensions below.

---

## Extension 1 — Hierarchical Checkpoints (Engineer Handoff)

Pure DPM hits a scaling wall: as the event log grows, every projection re-reads the entire trajectory. For long-horizon tasks (multi-day claims, multi-week engineering investigations) this becomes prohibitive.

Hierarchical Checkpoints solve this by extracting the NPU's KV-cache tensors at logical milestones (tool completions, context-pressure thresholds, trajectory shifts) and serializing them as Merkle-linked binary blobs. Each checkpoint stores a SHA-256 hash chained to the previous one — a verifiable chain of custody.

The unlocked capability is **bit-level session handoff**. When Engineer A finishes a session, Engineer B doesn't get briefed; they get a `session_id`. Their local LiteRT-LM client retrieves the checkpoint blob, memory-maps it directly into NPU KV-cache space, and bypasses the prefill phase entirely. The transfer is over gRPC + FlatBuffers (zero-copy), streamed in 2 MB frames over a persistent HTTP/2 connection. Hashing runs on hardware-accelerated ARMv8 Crypto Extensions in NEON registers — bypassing heap-allocation overhead of the standard EVP API.

This is the difference between "let me catch you up on what I've been doing" and "you now have the same brain I had thirty seconds ago."

---

## Extension 2 — Automated Audit Synthesis (Drift Detection)

Hierarchical Checkpoints introduce a new failure mode: a flawed projection can bake state errors into the permanent record. If a checkpoint silently drops a critical fact, the next engineer inherits a corrupted baseline.

Automated Audit Synthesis runs as a decoupled background pipeline. Lambda subscribes to MemoryDB keyspace notifications for new checkpoints; tasks flow through an SQS FIFO queue to a fleet of EC2 G6e workers (NVIDIA L40S, vLLM). Each worker pulls the raw event log from S3 and the target checkpoint from MemoryDB, runs a fresh deterministic projection over the raw events, and runs a comparator against the checkpoint state.

When drift is detected, the worker pushes a `CORRECTION` event back to the active session's Redis list. The local C++ runtime receives it through its persistent subscription, interrupts the main thread, discards the active KV-cache, appends the correction to the mmap log, and forces a fresh projection before the next `Predict()` returns. The agent cannot proceed on stale state.

Idea 1 (Hierarchical Checkpoints) and Idea 2 (Audit Synthesis) are structurally complementary: hierarchy makes audit tractable by reducing it to a delta check; audit makes hierarchy safe by guaranteeing checkpoint integrity. Deploying either alone is a worse system than deploying both.

---

## Key Features

- **Stateless by construction** — KV-cache is invalidated before every prefill; every decision is a pure function of the log plus the projection schema
- **Bit-level deterministic replay** — same log, same model version, byte-identical output (enforced by a 10-run unit test in CI)
- **Instant engineer handoff** — KV-cache tensor transfer via gRPC client-streaming, mmap'd directly into the receiving NPU
- **Hardware-accelerated integrity** — ARMv8 Crypto Extensions for SHA-256 Merkle roots, with BoringSSL/OpenSSL EVP fallback
- **Asynchronous audit pipeline** — heavy projection work runs on EC2 G6e GPUs; the edge device stays responsive
- **Compliance-ready storage tiers** — S3 (raw logs, Object Lock, permanent), MemoryDB (checkpoints, TTL'd), PostgreSQL (audit certificates, permanent)
- **Cross-platform inference** — Android, iOS, Web, Desktop, IoT — inheriting LiteRT-LM's hardware acceleration across CPU, GPU, and NPU
- **Broad model support** — Gemma 4, Llama, Phi-4, Qwen, and any model the underlying LiteRT-LM runtime supports

---

## Quick Start

The DPM runtime is consumed as a C++ library. The simplest test path is the LiteRT-LM CLI for the underlying inference, then driving it through the DPM substrate.

```bash
litert-lm run \
   --from-huggingface-repo=litert-community/gemma-4-E2B-it-litert-lm \
   gemma-4-E2B-it.litertlm \
   --memory-strategy=stateless_deterministic_projection \
   --config=./config/dpm.yaml \
   --prompt="What is the current liability gap for the warehouse equipment?"
```

A worked end-to-end example — a stateless claims adjuster with hierarchical checkpoints and audit hooks — lives in [`examples/stateless_claims_adjuster.cc`](./examples/stateless_claims_adjuster.cc).

---

## Documentation

The architecture is documented across three files, designed to be read in any order:

| File | Purpose |
|---|---|
| [`docs/_1_story.md`](./docs/_1_story.md) | Why each design decision was made — paper rationale, isolation principle, threading model, retention policy |
| [`docs/_2_structure.md`](./docs/_2_structure.md) | What to modify in the upstream LiteRT-LM tree to reach Phase 1 (paper), Phase 2 (checkpoints), Phase 3 (audit) — terse roadmap with `[src:...]` tags |
| [`docs/_3_source.md`](./docs/_3_source.md) | All C++, protobuf, prompt, and YAML artifacts, each tagged for grep-jump from the structure doc |

Cross-referencing is greppable: `grep -n 'src:cpp.sha256-neon' docs/*.md` jumps from any structure entry to its implementation.

---

## Supported Language APIs

DPM is implemented in the C++ runtime layer. Higher-level language bindings inherit it transparently.

| Language | Status | Best For | Documentation |
|---|---|---|---|
| C++ | Stable | High-performance native (DPM substrate lives here) | [C++ Guide](https://ai.google.dev/edge/litert-lm/cpp) |
| Kotlin | Stable | Android apps and JVM | [Android (Kotlin) Guide](https://ai.google.dev/edge/litert-lm/android) |
| Python | Stable | Prototyping and scripting | [Python Guide](https://ai.google.dev/edge/litert-lm/python) |
| Swift | In Dev | Native iOS and macOS | (coming soon) |

---

## Build From Source

Building DPM requires the upstream [LiteRT-LM build prerequisites](./docs/getting-started/build-and-run.md) plus:

- A C++ toolchain capable of `-march=armv8-a+crypto` (for the NEON SHA-256 kernel) — falls back to BoringSSL EVP otherwise
- gRPC C++ with FlatBuffers support
- AWS SDK for C++ (S3 multipart upload + MemoryDB Redis client)
- BoringSSL or OpenSSL ≥ 1.1.1

```bash
git clone https://github.com/maceip/LiteRT-LM.git --branch dpm
cd LiteRT-LM
cmake -B build -DLITERT_DPM=ON -DLITERT_DPM_NEON=ON
cmake --build build -j
```

The `LITERT_DPM=ON` flag enables the `EventSourcedLog`, `DPMProjector`, and `StatelessDecisionEngine` substrate. `LITERT_DPM_NEON=ON` enables the hardware-accelerated Merkle kernel; omit it to fall back to EVP.

---

## Releases

| Version | Highlights |
|---|---|
| v0.3.0 (substrate landed; bench in progress) | **Phase 3 — Replayable audit substrate.** Hash-first replay verifier (`projection_replay_auditor`); content-addressed audit certificates with ML-DSA post-quantum signatures (dynamic `liboqs`); fail-closed runtime decision gate; structured `CorrectionPayload` + correction barrier; half-open `[start, end)` global event ranges across manifests, replay, and gates; rollup child refs with write-time coverage validation; Merkle-DAG-linked certificate provenance; 17+ green substrate tests. |
| v0.2.0 (shipped) | **Phase 2 — Replayable memory substrate + first bench.** Append-only `EventSourcedLog` with CoW branching; binary `canonical_manifest` + BLAKE3/SHA-256 hashing; hierarchical `Level0` + `DeltaAppend` checkpoint codec; `CheckpointStore` + `LocalFilesystem` backend with idempotent durable writes; `MerkleDagStore` with provenance walk; `checkpoint_policy` (compatibility / thaw / compaction); two-phase `CheckpointUploadSession`; first DPM-vs-rolling-summary bench, 32 schema-validated `ScoreRow` records across 5 cases. |
| v0.1.0 (shipped) | **Phase 1 — DPM baseline.** `EventSourcedLog`, `DPMProjector`, `projection_prompt` with task-conditioned rebuild; `StatelessDecisionEngine`; `KvDtypePolicy` replay-safety gates; determinism CI harness. |

Tracks the upstream LiteRT-LM release line for the underlying inference engine. See [GitHub Releases](https://github.com/maceip/LiteRT-LM/releases) for the full history.

---

## Production Notes

The reference deployment target is AWS, eu-central-1 (Frankfurt) for EU data residency. The architecture mirrors cleanly to Azure (Cosmos DB + Change Feed + AKS) — see [`docs/_1_story.md`](./docs/_1_story.md) for the comparison.

DPM is a substrate, not a model. It works with any model the underlying LiteRT-LM runtime supports — Gemma, Llama, Phi-4, Qwen — provided that model is invoked at temperature 0 with a fixed seed during projection. The determinism guarantee is end-to-end only when both the substrate and the inference path are deterministic.

---

## Credits

Built on Google's [LiteRT-LM](https://github.com/google-ai-edge/LiteRT-LM). DPM architecture from Srinivasan et al., [arXiv:2604.20158](https://arxiv.org/abs/2604.20158). Hierarchical Checkpoints and Automated Audit Synthesis extensions developed in this repository.
