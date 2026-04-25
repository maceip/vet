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
// The default codec is INT8_PER_TOKEN absmax: for each (token, head)
// group, the maximum absolute value is captured as an fp16 scale, and the
// per-element values are rounded to int8 in [-127, 127]. Reconstruction is
//   value = q * (scale / 127.0)
// Per-token (rather than per-tensor) scaling preserves precision on outlier
// tokens that would otherwise dominate a global absmax.
//
// FP16 is the verified-fidelity escape hatch (no quantization) and is the
// path the determinism check uses to confirm "Thaw == Prefill" before
// accepting a quantized form for production traffic.
//
// INT4_CHANNEL is reserved for future implementation; the codec rejects
// it for now.

enum class KvDtype {
  kFp16 = 0,
  kInt8PerToken = 1,
  kInt4Channel = 2,  // not implemented; codec returns Unimplemented
};

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
