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

#include "runtime/platform/checkpoint/checkpoint_codec.h"

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "runtime/platform/checkpoint/kv_quantization.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

LayerKv MakeFp16Layer(uint32_t num_tokens, uint32_t num_heads,
                      uint32_t head_dim, uint16_t seed) {
  KvBlockShape shape{num_tokens, num_heads, head_dim};
  std::vector<uint16_t> k_bits, v_bits;
  k_bits.reserve(shape.Elements());
  v_bits.reserve(shape.Elements());
  for (size_t i = 0; i < shape.Elements(); ++i) {
    k_bits.push_back(static_cast<uint16_t>(seed + i));
    v_bits.push_back(static_cast<uint16_t>(seed + 0x4000 + i));
  }
  LayerKv layer;
  layer.k = *EncodeFp16(shape, k_bits);
  layer.v = *EncodeFp16(shape, v_bits);
  return layer;
}

LayerKv MakeInt8Layer(uint32_t num_tokens, uint32_t num_heads,
                      uint32_t head_dim, uint64_t seed) {
  KvBlockShape shape{num_tokens, num_heads, head_dim};
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
  std::vector<float> k(shape.Elements());
  std::vector<float> v(shape.Elements());
  for (auto& x : k) x = dist(rng);
  for (auto& x : v) x = dist(rng);
  LayerKv layer;
  layer.k = *EncodeInt8PerTokenFromFp32(shape, k);
  layer.v = *EncodeInt8PerTokenFromFp32(shape, v);
  return layer;
}

TEST(CheckpointCodecTest, Level0RoundTripFp16) {
  std::vector<LayerKv> layers;
  layers.push_back(MakeFp16Layer(/*num_tokens=*/4, 2, 8, 0x1000));
  layers.push_back(MakeFp16Layer(/*num_tokens=*/4, 2, 8, 0x2000));
  ASSERT_OK_AND_ASSIGN(CheckpointPayload p,
                       BuildLevel0(/*num_tokens=*/4, std::move(layers)));
  ASSERT_OK_AND_ASSIGN(std::string bytes, EncodeCheckpointPayload(p));
  ASSERT_OK_AND_ASSIGN(CheckpointPayload r, DecodeCheckpointPayload(bytes));

  EXPECT_EQ(r.scheme, CheckpointScheme::kLevel0);
  EXPECT_EQ(r.num_layers, 2);
  EXPECT_EQ(r.num_tokens, 4);
  ASSERT_EQ(r.layers.size(), 2);
  for (size_t li = 0; li < 2; ++li) {
    EXPECT_EQ(r.layers[li].k.shape.num_tokens, 4);
    EXPECT_EQ(r.layers[li].k.shape.num_heads, 2);
    EXPECT_EQ(r.layers[li].k.shape.head_dim, 8);
    EXPECT_EQ(r.layers[li].k.payload, p.layers[li].k.payload);
    EXPECT_EQ(r.layers[li].v.payload, p.layers[li].v.payload);
  }
}

TEST(CheckpointCodecTest, DeltaAppendRoundTripInt8) {
  std::vector<LayerKv> appended;
  appended.push_back(MakeInt8Layer(/*num_tokens=*/3, 2, 8, 0xAA));
  appended.push_back(MakeInt8Layer(/*num_tokens=*/3, 2, 8, 0xBB));
  ASSERT_OK_AND_ASSIGN(CheckpointPayload p,
                       BuildDeltaAppend(/*num_appended_tokens=*/3,
                                        std::move(appended)));
  ASSERT_OK_AND_ASSIGN(std::string bytes, EncodeCheckpointPayload(p));
  ASSERT_OK_AND_ASSIGN(CheckpointPayload r, DecodeCheckpointPayload(bytes));
  EXPECT_EQ(r.scheme, CheckpointScheme::kDeltaAppend);
  EXPECT_EQ(r.num_tokens, 3);
}

TEST(CheckpointCodecTest, ReconstructFromChainEqualsFlatLevel0_Int8) {
  // Build a flat Level0 and a Level0+Delta chain that should reconstruct
  // to byte-equivalent KV state (within int8 quantization noise).
  KvBlockShape full_shape{/*num_tokens=*/5, 1, 4};
  std::vector<float> k_full(full_shape.Elements(), 0.0f);
  std::vector<float> v_full(full_shape.Elements(), 0.0f);
  std::mt19937_64 rng(20260420);
  std::uniform_real_distribution<float> dist(-0.7f, 0.7f);
  for (auto& x : k_full) x = dist(rng);
  for (auto& x : v_full) x = dist(rng);

  // Per-token chunks: first 3 tokens -> base Level0, last 2 tokens -> delta.
  KvBlockShape base_shape{3, 1, 4};
  KvBlockShape tail_shape{2, 1, 4};
  std::vector<float> k_base(k_full.begin(), k_full.begin() + 3 * 4);
  std::vector<float> v_base(v_full.begin(), v_full.begin() + 3 * 4);
  std::vector<float> k_tail(k_full.begin() + 3 * 4, k_full.end());
  std::vector<float> v_tail(v_full.begin() + 3 * 4, v_full.end());

  LayerKv base_layer{
      *EncodeInt8PerTokenFromFp32(base_shape, k_base),
      *EncodeInt8PerTokenFromFp32(base_shape, v_base),
  };
  LayerKv tail_layer{
      *EncodeInt8PerTokenFromFp32(tail_shape, k_tail),
      *EncodeInt8PerTokenFromFp32(tail_shape, v_tail),
  };
  LayerKv full_layer{
      *EncodeInt8PerTokenFromFp32(full_shape, k_full),
      *EncodeInt8PerTokenFromFp32(full_shape, v_full),
  };

  ASSERT_OK_AND_ASSIGN(CheckpointPayload base,
                       BuildLevel0(/*num_tokens=*/3, {base_layer}));
  ASSERT_OK_AND_ASSIGN(CheckpointPayload delta,
                       BuildDeltaAppend(/*num_appended_tokens=*/2,
                                        {tail_layer}));
  ASSERT_OK_AND_ASSIGN(CheckpointPayload reconstructed,
                       ReconstructFromChain({base, delta}));

  EXPECT_EQ(reconstructed.scheme, CheckpointScheme::kLevel0);
  EXPECT_EQ(reconstructed.num_tokens, 5);
  ASSERT_EQ(reconstructed.layers.size(), 1);
  EXPECT_EQ(reconstructed.layers[0].k.shape.num_tokens, 5);
  EXPECT_EQ(reconstructed.layers[0].v.shape.num_tokens, 5);

  // The reconstructed K/V tensors should round-trip to the same float
  // values as encoding the full tensor directly (modulo the per-token
  // absmax scales being computed independently for each chunk; this is
  // expected and tests that the chain delivers the same per-chunk result
  // a chained encoder would).
  ASSERT_OK_AND_ASSIGN(std::vector<float> reconstructed_k,
                       DecodeInt8PerTokenToFp32(reconstructed.layers[0].k));
  ASSERT_OK_AND_ASSIGN(std::vector<float> chunked_k_base,
                       DecodeInt8PerTokenToFp32(base_layer.k));
  ASSERT_OK_AND_ASSIGN(std::vector<float> chunked_k_tail,
                       DecodeInt8PerTokenToFp32(tail_layer.k));
  std::vector<float> expected_k = chunked_k_base;
  expected_k.insert(expected_k.end(), chunked_k_tail.begin(),
                    chunked_k_tail.end());
  EXPECT_EQ(reconstructed_k, expected_k);
}

TEST(CheckpointCodecTest, ReconstructFromChainHandlesCompactionReset) {
  // chain = [L0(a), Delta(b), L0(c)] should reconstruct to L0(c) only.
  std::vector<LayerKv> a = {MakeFp16Layer(2, 1, 4, 0x100)};
  std::vector<LayerKv> b = {MakeFp16Layer(1, 1, 4, 0x200)};
  std::vector<LayerKv> c = {MakeFp16Layer(7, 1, 4, 0x300)};
  ASSERT_OK_AND_ASSIGN(auto p_a, BuildLevel0(2, std::move(a)));
  ASSERT_OK_AND_ASSIGN(auto p_b, BuildDeltaAppend(1, std::move(b)));
  ASSERT_OK_AND_ASSIGN(auto p_c, BuildLevel0(7, std::move(c)));
  ASSERT_OK_AND_ASSIGN(CheckpointPayload r,
                       ReconstructFromChain({p_a, p_b, p_c}));
  EXPECT_EQ(r.num_tokens, 7);
}

TEST(CheckpointCodecTest, RejectsTrailingBytes) {
  std::vector<LayerKv> layers = {MakeFp16Layer(1, 1, 4, 0x111)};
  ASSERT_OK_AND_ASSIGN(auto p, BuildLevel0(1, std::move(layers)));
  ASSERT_OK_AND_ASSIGN(std::string bytes, EncodeCheckpointPayload(p));
  bytes.push_back('!');
  EXPECT_FALSE(DecodeCheckpointPayload(bytes).ok());
}

TEST(CheckpointCodecTest, RejectsMissingMagic) {
  std::string garbage = "not a checkpoint";
  EXPECT_FALSE(DecodeCheckpointPayload(garbage).ok());
}

TEST(CheckpointCodecTest, RejectsLayerCountMismatch) {
  CheckpointPayload p;
  p.scheme = CheckpointScheme::kLevel0;
  p.num_layers = 2;            // claims 2
  p.num_tokens = 1;
  p.layers.push_back(MakeFp16Layer(1, 1, 4, 0x123));  // but only 1
  EXPECT_FALSE(EncodeCheckpointPayload(p).ok());
}

TEST(CheckpointCodecTest, ReconstructRejectsHeadLayoutChangeMidChain) {
  std::vector<LayerKv> base = {MakeFp16Layer(1, 1, 4, 0x10)};
  std::vector<LayerKv> mismatched_delta = {MakeFp16Layer(1, 1, 8, 0x20)};
  ASSERT_OK_AND_ASSIGN(auto p_a, BuildLevel0(1, std::move(base)));
  ASSERT_OK_AND_ASSIGN(auto p_b,
                       BuildDeltaAppend(1, std::move(mismatched_delta)));
  EXPECT_FALSE(ReconstructFromChain({p_a, p_b}).ok());
}

TEST(CheckpointCodecTest, ReconstructRejectsEmptyChain) {
  EXPECT_FALSE(ReconstructFromChain({}).ok());
}

TEST(CheckpointCodecTest, ReconstructRejectsDeltaFirst) {
  std::vector<LayerKv> delta = {MakeFp16Layer(1, 1, 4, 0x55)};
  ASSERT_OK_AND_ASSIGN(auto p_b, BuildDeltaAppend(1, std::move(delta)));
  EXPECT_FALSE(ReconstructFromChain({p_b}).ok());
}

}  // namespace
}  // namespace litert::lm
