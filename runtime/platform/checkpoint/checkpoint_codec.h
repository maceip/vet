// Copyright 2026 Google LLC.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_CHECKPOINT_CODEC_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_CHECKPOINT_CODEC_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/checkpoint/kv_quantization.h"

namespace litert::lm {

// On-wire / at-rest encoding for a single hierarchical checkpoint payload.
// The CheckpointAbi header (defined in runtime/proto/checkpoint.proto)
// wraps this payload; the codec below is concerned only with the body
// bytes that body_hash is computed over.
//
// Two schemes are supported in Phase 2:
//
//   Level0 — a full snapshot covering [0, num_tokens). Self-contained;
//     no parent_hash is required at decode time. Used for genesis and
//     for compaction outputs.
//
//   DeltaAppend — the common case where the agent appended
//     `num_appended_tokens` tokens to the tail since the parent
//     checkpoint. Reconstruction concatenates parent's per-layer K/V
//     tensors with the appended slices. Compatible with autoregressive
//     decode and any append-only token interaction.
//
// A sparse-pages delta scheme is reserved for the future general case
// (rewriting non-tail pages); decoders return Unimplemented for it.

enum class CheckpointScheme : uint8_t {
  kLevel0 = 0,
  kDeltaAppend = 1,
  kDeltaSparsePages = 2,  // not implemented in Phase 2
};

// In-memory representation of a single layer's KV tensors. K and V are
// independently encoded so the dtype can differ if a deployment chooses
// (e.g. fp16 K, int8 V). Phase 2 default uses the same dtype for both.
struct LayerKv {
  EncodedKvBlock k;
  EncodedKvBlock v;
};

// Decoded payload shape. For Level0, num_tokens is the absolute coverage
// length. For DeltaAppend, num_tokens is the number of *newly appended*
// tokens; the consumer must concatenate this against the parent's tensor.
struct CheckpointPayload {
  CheckpointScheme scheme = CheckpointScheme::kLevel0;
  uint32_t num_layers = 0;
  uint32_t num_tokens = 0;
  std::vector<LayerKv> layers;
};

// Round trip. The bytes returned start with a magic header so concat
// errors and partial payloads are detected; partner's transport layer
// should treat the bytes as opaque and re-hash them with body_hash.
absl::StatusOr<std::string> EncodeCheckpointPayload(
    const CheckpointPayload& payload);

absl::StatusOr<CheckpointPayload> DecodeCheckpointPayload(
    absl::string_view bytes);

// Convenience constructor for a Level0 payload from per-layer EncodedKvBlocks
// already in their target dtype. num_layers is implied by `layers.size()`.
absl::StatusOr<CheckpointPayload> BuildLevel0(uint32_t num_tokens,
                                              std::vector<LayerKv> layers);

// Convenience constructor for a DeltaAppend payload.
absl::StatusOr<CheckpointPayload> BuildDeltaAppend(
    uint32_t num_appended_tokens, std::vector<LayerKv> appended_layers);

// Reconstructs the live KV tensor for a session by replaying a chain of
// checkpoint payloads from a Level0 base through zero or more DeltaAppend
// payloads. Caller is responsible for verifying that the chain is
// hash-consistent (see runtime/platform/provenance for the DAG side).
//
// Layouts must match: every payload in the chain must have the same
// num_layers and per-layer (num_heads, head_dim). num_tokens accumulates
// across the chain.
absl::StatusOr<CheckpointPayload> ReconstructFromChain(
    const std::vector<CheckpointPayload>& chain);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_CHECKPOINT_CODEC_H_
