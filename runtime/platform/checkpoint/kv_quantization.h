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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_KV_QUANTIZATION_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_KV_QUANTIZATION_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/types/span.h"  // from @com_google_absl

namespace litert::lm {

// KV-cache transport quantization.
//
// The live in-memory KV cache stays at the model's native precision (fp16,
// bf16, etc.). When a checkpoint is encoded for at-rest storage or wire
// transport, KV blocks are run through the codec below so that the on-wire
// payload is significantly smaller than the live representation.
//
// **Replay-safety contract.**
//
// kFp16 is the only codec considered replay-safe by default: the bytes
// round-trip exactly, so a thawed cache is bit-identical to the source
// cache. The Phase 1 deterministic-replay property therefore extends
// across thaw boundaries when fp16 is used.
//
// kInt8PerToken is a **lossy** transport encoding. Per-(token, head) absmax
// scaling minimizes the loss (bounded by absmax/254 per element) and
// preserves outlier-light tokens, but a thawed int8 cache is NOT
// bit-identical to the source. Whether the resulting decoded behavior
// matches the source is a *target-backend* and *target-model* property
// that the runtime cannot answer without a thaw-equivalence test on the
// real inference stack. Callers must therefore make an explicit policy
// choice via PickReplaySafeKvDtype() / RequireReplaySafeKvDtype() below
// before any int8 bytes leave the producer.
//
// In summary:
//   - kFp16:           always replay-safe; default.
//   - kInt8PerToken:   replay-unsafe by default; opt-in only after a
//                      thaw-equivalence test passes for the (model,
//                      backend, dtype) triple.
//   - kInt4Channel:    reserved for future implementation; codec rejects.

enum class KvDtype {
  kFp16 = 0,
  kInt8PerToken = 1,
  kInt4Channel = 2,  // not implemented; codec returns Unimplemented
};

// Whether a deployment has explicitly approved a transport codec for a
// (model, backend, dtype) triple by running and passing a thaw-equivalence
// test. Replay-safe paths must consult this before encoding KV bytes; the
// runtime never silently picks a lossy codec.
struct KvDtypePolicy {
  // Replay-safe-only mode: producers may only use kFp16. This is the
  // safe default for any path that participates in deterministic replay.
  bool require_replay_safe = true;

  // The dtype the deployment has audited and approved for this session.
  // Only honored when require_replay_safe is false. Defaults to kFp16.
  KvDtype approved_dtype = KvDtype::kFp16;
};

// Returns kFp16 if `policy` requires replay-safety; otherwise returns
// `policy.approved_dtype`. Callers should use this rather than hardcoding
// a codec choice so the replay-safe default is honored in every code path.
KvDtype PickReplaySafeKvDtype(const KvDtypePolicy& policy);

// Returns InvalidArgument if `dtype` would be unsafe under `policy` (i.e.
// the policy requires replay-safety and `dtype` is not kFp16). Use at the
// boundary where a caller proposes a codec choice.
absl::Status RequireReplaySafeKvDtype(const KvDtypePolicy& policy,
                                      KvDtype dtype);

struct KvBlockShape {
  // Number of token positions covered by this block.
  uint32_t num_tokens = 0;
  // Number of KV heads (post-GQA collapse, i.e. the actual stored heads).
  uint32_t num_heads = 0;
  // Per-head dimension.
  uint32_t head_dim = 0;
  // Returns the number of elements expected in the unquantized block.
  size_t Elements() const {
    return static_cast<size_t>(num_tokens) * num_heads * head_dim;
  }
};

// Encoded form. payload bytes layout:
//   kFp16:           num_tokens * num_heads * head_dim * 2 bytes
//   kInt8PerToken:   num_tokens * num_heads * head_dim bytes (int8) +
//                    num_tokens * num_heads * 2 bytes (fp16 scales,
//                    indexed by token-major then head)
struct EncodedKvBlock {
  KvDtype dtype = KvDtype::kFp16;
  KvBlockShape shape;
  std::string payload;
};

// Round-trip primitives. The fp16 path stores the input bytes verbatim
// (callers supply already-fp16 data as raw uint16_t per element).
absl::StatusOr<EncodedKvBlock> EncodeFp16(KvBlockShape shape,
                                          absl::Span<const uint16_t> values);
absl::StatusOr<std::vector<uint16_t>> DecodeFp16(const EncodedKvBlock& block);

// Encodes from a vector of float32 source values. INT8_PER_TOKEN computes
// per-(token, head) absmax in float, scales to int8, and stores the
// half-precision scale. Rejects NaN/Inf at the boundary.
absl::StatusOr<EncodedKvBlock> EncodeInt8PerTokenFromFp32(
    KvBlockShape shape, absl::Span<const float> values);

// Decodes back to float32. The reconstruction error per element is bounded
// by (absmax / 127.0); see DequantizationErrorBound.
absl::StatusOr<std::vector<float>> DecodeInt8PerTokenToFp32(
    const EncodedKvBlock& block);

// Worst-case absolute error introduced by INT8_PER_TOKEN dequantization
// for a given absmax. Useful for the determinism gate: live tokens whose
// absmax exceeds a configured budget can fall back to fp16 for that group.
float DequantizationErrorBound(float absmax);

// Theoretical post-encode size in bytes for a given dtype and shape.
size_t EncodedSizeBytes(KvDtype dtype, const KvBlockShape& shape);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_KV_QUANTIZATION_H_
