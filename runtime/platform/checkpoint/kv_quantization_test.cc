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
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "gtest/gtest.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

constexpr KvBlockShape kSmallShape = {/*num_tokens=*/4, /*num_heads=*/2,
                                      /*head_dim=*/8};

TEST(KvQuantizationTest, EncodedSizeBytesMatchesLayout) {
  EXPECT_EQ(EncodedSizeBytes(KvDtype::kFp16, kSmallShape),
            4 * 2 * 8 * 2);
  EXPECT_EQ(EncodedSizeBytes(KvDtype::kInt8PerToken, kSmallShape),
            4 * 2 * 8 + 4 * 2 * 2);
  EXPECT_EQ(EncodedSizeBytes(KvDtype::kInt4Channel, kSmallShape), 0);
}

TEST(KvQuantizationTest, Fp16RoundTripIsBitExact) {
  // The fp16 path stores raw fp16 bits verbatim.
  std::vector<uint16_t> input;
  input.reserve(kSmallShape.Elements());
  for (size_t i = 0; i < kSmallShape.Elements(); ++i) {
    input.push_back(static_cast<uint16_t>(0x3C00 + i));  // 1.0 + small offsets
  }
  ASSERT_OK_AND_ASSIGN(EncodedKvBlock encoded,
                       EncodeFp16(kSmallShape, input));
  EXPECT_EQ(encoded.dtype, KvDtype::kFp16);
  EXPECT_EQ(encoded.payload.size(),
            EncodedSizeBytes(KvDtype::kFp16, kSmallShape));
  ASSERT_OK_AND_ASSIGN(std::vector<uint16_t> decoded, DecodeFp16(encoded));
  EXPECT_EQ(decoded, input);
}

TEST(KvQuantizationTest, Int8RoundTripIsBoundedError) {
  // Build a deterministic float input with mixed magnitudes.
  std::mt19937 rng(20260420);
  std::uniform_real_distribution<float> dist(-3.0f, 3.0f);
  std::vector<float> input(kSmallShape.Elements());
  for (float& v : input) v = dist(rng);

  ASSERT_OK_AND_ASSIGN(EncodedKvBlock encoded,
                       EncodeInt8PerTokenFromFp32(kSmallShape, input));
  EXPECT_EQ(encoded.dtype, KvDtype::kInt8PerToken);
  EXPECT_EQ(encoded.payload.size(),
            EncodedSizeBytes(KvDtype::kInt8PerToken, kSmallShape));

  ASSERT_OK_AND_ASSIGN(std::vector<float> decoded,
                       DecodeInt8PerTokenToFp32(encoded));
  ASSERT_EQ(decoded.size(), input.size());

  // Per-token absmax is bounded by 3.0 here, so the dequantization error
  // bound is < 0.012 per element. The max observed error must respect that.
  const float bound = DequantizationErrorBound(3.0f);
  float max_err = 0.0f;
  for (size_t i = 0; i < input.size(); ++i) {
    max_err = std::max(max_err, std::fabs(input[i] - decoded[i]));
  }
  EXPECT_LE(max_err, bound + 1e-6f);
}

TEST(KvQuantizationTest, Int8HandlesAllZerosWithoutDivByZero) {
  std::vector<float> zeros(kSmallShape.Elements(), 0.0f);
  ASSERT_OK_AND_ASSIGN(EncodedKvBlock encoded,
                       EncodeInt8PerTokenFromFp32(kSmallShape, zeros));
  ASSERT_OK_AND_ASSIGN(std::vector<float> decoded,
                       DecodeInt8PerTokenToFp32(encoded));
  for (float v : decoded) EXPECT_EQ(v, 0.0f);
}

TEST(KvQuantizationTest, Int8RejectsNanAndInf) {
  std::vector<float> input(kSmallShape.Elements(), 0.5f);
  input[3] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(EncodeInt8PerTokenFromFp32(kSmallShape, input).ok());
  input[3] = std::numeric_limits<float>::infinity();
  EXPECT_FALSE(EncodeInt8PerTokenFromFp32(kSmallShape, input).ok());
}

TEST(KvQuantizationTest, Int8PerTokenScalingPreservesOutlierTokens) {
  // Two tokens, one with large magnitude and one with small. Per-tensor
  // absmax would crush the small-magnitude token to zero; per-token does
  // not.
  KvBlockShape shape = {/*num_tokens=*/2, /*num_heads=*/1, /*head_dim=*/4};
  std::vector<float> input = {10.0f, -5.0f, 7.0f, -3.0f,    // big token
                              0.01f, -0.02f, 0.03f, -0.01f};  // small token
  ASSERT_OK_AND_ASSIGN(EncodedKvBlock encoded,
                       EncodeInt8PerTokenFromFp32(shape, input));
  ASSERT_OK_AND_ASSIGN(std::vector<float> decoded,
                       DecodeInt8PerTokenToFp32(encoded));
  // Big-token bound: 10/254 = 0.0394
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_LE(std::fabs(input[i] - decoded[i]),
              DequantizationErrorBound(10.0f) + 1e-6f);
  }
  // Small-token bound: 0.03/254 = ~1.18e-4. Per-tensor absmax would have
  // scaled by 10 here and crushed everything; verify it didn't.
  for (size_t i = 4; i < 8; ++i) {
    EXPECT_LE(std::fabs(input[i] - decoded[i]),
              DequantizationErrorBound(0.03f) + 1e-6f);
  }
}

TEST(KvQuantizationTest, ShapeMismatchRejected) {
  std::vector<float> too_few(kSmallShape.Elements() - 1, 0.0f);
  EXPECT_FALSE(EncodeInt8PerTokenFromFp32(kSmallShape, too_few).ok());
  std::vector<uint16_t> too_few_fp16(kSmallShape.Elements() - 1, 0);
  EXPECT_FALSE(EncodeFp16(kSmallShape, too_few_fp16).ok());
}

TEST(KvQuantizationTest, ZeroShapeRejected) {
  KvBlockShape bad = {0, 1, 1};
  std::vector<float> empty;
  EXPECT_FALSE(EncodeInt8PerTokenFromFp32(bad, empty).ok());
}

TEST(KvQuantizationTest, DequantizationErrorBoundIsCorrect) {
  EXPECT_NEAR(DequantizationErrorBound(127.0f * 254.0f / 254.0f), 0.5f, 1e-6f);
  EXPECT_NEAR(DequantizationErrorBound(0.0f), 0.0f, 1e-6f);
}

}  // namespace
}  // namespace litert::lm
