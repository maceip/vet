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

#include "runtime/platform/checkpoint/kv_quantization.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/types/span.h"  // from @com_google_absl

namespace litert::lm {
namespace {

// Convert a single fp32 value to IEEE 754 binary16 (half) bits using
// round-half-to-even. Preserves NaN, infinity, and zero; flushes subnormal
// inputs that underflow fp16's exponent range to zero. KV values are post-
// softmax / post-projection and should not contain such extreme values, so
// the lossy edge cases here are acceptable for transport encoding.
uint16_t Fp32ToFp16(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = (bits >> 31) & 0x1;
  const uint32_t exp32 = (bits >> 23) & 0xFF;
  const uint32_t frac32 = bits & 0x7FFFFF;

  uint16_t out_sign = static_cast<uint16_t>(sign << 15);
  // Special: zero
  if (exp32 == 0 && frac32 == 0) {
    return out_sign;
  }
  // Special: NaN / infinity
  if (exp32 == 0xFF) {
    if (frac32 == 0) {
      return static_cast<uint16_t>(out_sign | 0x7C00);  // +/-inf
    }
    // NaN: preserve a non-zero significand bit
    return static_cast<uint16_t>(out_sign | 0x7C00 |
                                 (frac32 >> 13 ? frac32 >> 13 : 0x1));
  }

  const int32_t exp_unbiased = static_cast<int32_t>(exp32) - 127;
  const int32_t exp16 = exp_unbiased + 15;
  if (exp16 >= 0x1F) {
    // Overflow: saturate to fp16 +/-inf.
    return static_cast<uint16_t>(out_sign | 0x7C00);
  }
  if (exp16 <= 0) {
    // Underflow: flush to zero.
    return out_sign;
  }
  // Round-half-to-even on the bottom 13 bits of frac32.
  const uint32_t round_bit = frac32 & 0x1000;
  const uint32_t sticky = frac32 & 0xFFF;
  uint32_t mantissa = frac32 >> 13;
  if (round_bit && (sticky || (mantissa & 0x1))) {
    ++mantissa;
    if (mantissa == 0x400) {
      // Carry into exponent.
      mantissa = 0;
      const int32_t carried_exp = exp16 + 1;
      if (carried_exp >= 0x1F) {
        return static_cast<uint16_t>(out_sign | 0x7C00);
      }
      return static_cast<uint16_t>(out_sign |
                                   (static_cast<uint16_t>(carried_exp) << 10));
    }
  }
  return static_cast<uint16_t>(out_sign |
                               (static_cast<uint16_t>(exp16) << 10) |
                               static_cast<uint16_t>(mantissa));
}

float Fp16ToFp32(uint16_t bits) {
  const uint32_t sign = (bits >> 15) & 0x1;
  const uint32_t exp16 = (bits >> 10) & 0x1F;
  const uint32_t frac16 = bits & 0x3FF;
  uint32_t out_bits;
  if (exp16 == 0) {
    if (frac16 == 0) {
      out_bits = sign << 31;
    } else {
      // Subnormal fp16 -> normalize into fp32.
      uint32_t mantissa = frac16;
      int32_t exp32 = -14;
      while ((mantissa & 0x400) == 0) {
        mantissa <<= 1;
        --exp32;
      }
      mantissa &= 0x3FF;
      const uint32_t out_exp = static_cast<uint32_t>(exp32 + 127);
      out_bits = (sign << 31) | (out_exp << 23) | (mantissa << 13);
    }
  } else if (exp16 == 0x1F) {
    out_bits = (sign << 31) | (0xFF << 23) | (frac16 << 13);
  } else {
    const uint32_t out_exp = exp16 - 15 + 127;
    out_bits = (sign << 31) | (out_exp << 23) | (frac16 << 13);
  }
  float out;
  std::memcpy(&out, &out_bits, sizeof(out));
  return out;
}

absl::Status ValidateShape(const KvBlockShape& shape) {
  if (shape.num_tokens == 0 || shape.num_heads == 0 || shape.head_dim == 0) {
    return absl::InvalidArgumentError(
        "KvBlockShape requires positive num_tokens, num_heads, head_dim.");
  }
  return absl::OkStatus();
}

}  // namespace

size_t EncodedSizeBytes(KvDtype dtype, const KvBlockShape& shape) {
  const size_t per_token_per_head = static_cast<size_t>(shape.head_dim);
  const size_t groups =
      static_cast<size_t>(shape.num_tokens) * shape.num_heads;
  switch (dtype) {
    case KvDtype::kFp16:
      return groups * per_token_per_head * sizeof(uint16_t);
    case KvDtype::kInt8PerToken:
      return groups * per_token_per_head + groups * sizeof(uint16_t);
    case KvDtype::kInt4Channel:
      return 0;
  }
  return 0;
}

float DequantizationErrorBound(float absmax) {
  // Symmetric int8 with 127 bins maps [-absmax, absmax] uniformly; the
  // worst-case quantization error is half a bin width = absmax / 254.
  return absmax / 254.0f;
}

absl::StatusOr<EncodedKvBlock> EncodeFp16(KvBlockShape shape,
                                          absl::Span<const uint16_t> values) {
  if (auto status = ValidateShape(shape); !status.ok()) return status;
  if (values.size() != shape.Elements()) {
    return absl::InvalidArgumentError(
        absl::StrCat("EncodeFp16 expected ", shape.Elements(),
                     " values, got ", values.size(), "."));
  }
  EncodedKvBlock block;
  block.dtype = KvDtype::kFp16;
  block.shape = shape;
  block.payload.assign(reinterpret_cast<const char*>(values.data()),
                       values.size() * sizeof(uint16_t));
  return block;
}

absl::StatusOr<std::vector<uint16_t>> DecodeFp16(const EncodedKvBlock& block) {
  if (block.dtype != KvDtype::kFp16) {
    return absl::InvalidArgumentError("DecodeFp16 called on non-fp16 block.");
  }
  if (auto status = ValidateShape(block.shape); !status.ok()) return status;
  const size_t expected = block.shape.Elements() * sizeof(uint16_t);
  if (block.payload.size() != expected) {
    return absl::DataLossError(
        absl::StrCat("Fp16 payload size mismatch: got ", block.payload.size(),
                     " expected ", expected));
  }
  std::vector<uint16_t> out(block.shape.Elements());
  std::memcpy(out.data(), block.payload.data(), expected);
  return out;
}

absl::StatusOr<EncodedKvBlock> EncodeInt8PerTokenFromFp32(
    KvBlockShape shape, absl::Span<const float> values) {
  if (auto status = ValidateShape(shape); !status.ok()) return status;
  if (values.size() != shape.Elements()) {
    return absl::InvalidArgumentError(
        absl::StrCat("EncodeInt8PerTokenFromFp32 expected ", shape.Elements(),
                     " values, got ", values.size(), "."));
  }

  const size_t groups =
      static_cast<size_t>(shape.num_tokens) * shape.num_heads;
  const size_t group_size = shape.head_dim;
  const size_t int8_bytes = groups * group_size;
  const size_t scale_bytes = groups * sizeof(uint16_t);

  EncodedKvBlock block;
  block.dtype = KvDtype::kInt8PerToken;
  block.shape = shape;
  block.payload.resize(int8_bytes + scale_bytes);

  int8_t* int8_out = reinterpret_cast<int8_t*>(block.payload.data());
  uint16_t* scales_out =
      reinterpret_cast<uint16_t*>(block.payload.data() + int8_bytes);

  for (size_t g = 0; g < groups; ++g) {
    const float* src = values.data() + g * group_size;
    float absmax = 0.0f;
    for (size_t i = 0; i < group_size; ++i) {
      const float v = src[i];
      if (!std::isfinite(v)) {
        return absl::InvalidArgumentError(
            "EncodeInt8PerTokenFromFp32 rejects NaN/Inf inputs.");
      }
      const float a = std::fabs(v);
      if (a > absmax) absmax = a;
    }
    scales_out[g] = Fp32ToFp16(absmax);
    int8_t* dst = int8_out + g * group_size;
    if (absmax == 0.0f) {
      for (size_t i = 0; i < group_size; ++i) dst[i] = 0;
      continue;
    }
    const float inv_scale = 127.0f / absmax;
    for (size_t i = 0; i < group_size; ++i) {
      const float scaled = src[i] * inv_scale;
      // Round half-to-even. Matches TensorFlow / PyTorch quantization.
      float rounded = std::nearbyint(scaled);
      if (rounded > 127.0f) rounded = 127.0f;
      if (rounded < -127.0f) rounded = -127.0f;
      dst[i] = static_cast<int8_t>(rounded);
    }
  }
  return block;
}

absl::StatusOr<std::vector<float>> DecodeInt8PerTokenToFp32(
    const EncodedKvBlock& block) {
  if (block.dtype != KvDtype::kInt8PerToken) {
    return absl::InvalidArgumentError(
        "DecodeInt8PerTokenToFp32 called on non-int8 block.");
  }
  if (auto status = ValidateShape(block.shape); !status.ok()) return status;
  const size_t groups =
      static_cast<size_t>(block.shape.num_tokens) * block.shape.num_heads;
  const size_t group_size = block.shape.head_dim;
  const size_t expected =
      groups * group_size + groups * sizeof(uint16_t);
  if (block.payload.size() != expected) {
    return absl::DataLossError(
        absl::StrCat("Int8 payload size mismatch: got ", block.payload.size(),
                     " expected ", expected));
  }
  const int8_t* int8_in =
      reinterpret_cast<const int8_t*>(block.payload.data());
  const uint16_t* scales_in = reinterpret_cast<const uint16_t*>(
      block.payload.data() + groups * group_size);

  std::vector<float> out(block.shape.Elements());
  for (size_t g = 0; g < groups; ++g) {
    const float scale = Fp16ToFp32(scales_in[g]);
    const float per_step = scale / 127.0f;
    const int8_t* src = int8_in + g * group_size;
    float* dst = out.data() + g * group_size;
    for (size_t i = 0; i < group_size; ++i) {
      dst[i] = static_cast<float>(src[i]) * per_step;
    }
  }
  return out;
}

}  // namespace litert::lm
