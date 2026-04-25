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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/checkpoint/kv_quantization.h"

namespace litert::lm {
namespace {

constexpr std::array<char, 9> kPayloadMagic = {'D', 'P', 'M', 'C', 'K',
                                               'P', 'T', '1', '\n'};

// Small fixed-width integer helpers. The on-wire format is little-endian
// throughout to match the Phase 1 eventlog framing.
void AppendU8(uint8_t v, std::string* out) {
  out->push_back(static_cast<char>(v));
}
void AppendU32(uint32_t v, std::string* out) {
  out->push_back(static_cast<char>(v & 0xff));
  out->push_back(static_cast<char>((v >> 8) & 0xff));
  out->push_back(static_cast<char>((v >> 16) & 0xff));
  out->push_back(static_cast<char>((v >> 24) & 0xff));
}

absl::StatusOr<uint8_t> ReadU8(absl::string_view& view) {
  if (view.empty()) {
    return absl::DataLossError("checkpoint payload truncated reading u8.");
  }
  uint8_t v = static_cast<uint8_t>(view.front());
  view.remove_prefix(1);
  return v;
}

absl::StatusOr<uint32_t> ReadU32(absl::string_view& view) {
  if (view.size() < 4) {
    return absl::DataLossError("checkpoint payload truncated reading u32.");
  }
  uint32_t v =
      static_cast<uint32_t>(static_cast<unsigned char>(view[0])) |
      (static_cast<uint32_t>(static_cast<unsigned char>(view[1])) << 8) |
      (static_cast<uint32_t>(static_cast<unsigned char>(view[2])) << 16) |
      (static_cast<uint32_t>(static_cast<unsigned char>(view[3])) << 24);
  view.remove_prefix(4);
  return v;
}

// Per-block on-wire framing:
//   dtype: u8
//   num_tokens: u32
//   num_heads: u32
//   head_dim: u32
//   payload_size: u32
//   payload: bytes
absl::Status EncodeBlock(const EncodedKvBlock& block, std::string* out) {
  if (block.payload.size() > 0xFFFFFFFFu) {
    return absl::ResourceExhaustedError(
        "checkpoint block payload exceeds u32 size cap.");
  }
  AppendU8(static_cast<uint8_t>(block.dtype), out);
  AppendU32(block.shape.num_tokens, out);
  AppendU32(block.shape.num_heads, out);
  AppendU32(block.shape.head_dim, out);
  AppendU32(static_cast<uint32_t>(block.payload.size()), out);
  out->append(block.payload);
  return absl::OkStatus();
}

absl::StatusOr<EncodedKvBlock> DecodeBlock(absl::string_view& view) {
  EncodedKvBlock block;
  auto dtype_byte = ReadU8(view);
  if (!dtype_byte.ok()) return dtype_byte.status();
  switch (*dtype_byte) {
    case static_cast<uint8_t>(KvDtype::kFp16):
      block.dtype = KvDtype::kFp16;
      break;
    case static_cast<uint8_t>(KvDtype::kInt8PerToken):
      block.dtype = KvDtype::kInt8PerToken;
      break;
    case static_cast<uint8_t>(KvDtype::kInt4Channel):
      return absl::UnimplementedError(
          "kInt4Channel decoding is not implemented in Phase 2.");
    default:
      return absl::DataLossError(
          absl::StrCat("unknown KvDtype byte ", *dtype_byte));
  }
  auto nt = ReadU32(view); if (!nt.ok()) return nt.status();
  auto nh = ReadU32(view); if (!nh.ok()) return nh.status();
  auto hd = ReadU32(view); if (!hd.ok()) return hd.status();
  auto sz = ReadU32(view); if (!sz.ok()) return sz.status();
  block.shape.num_tokens = *nt;
  block.shape.num_heads = *nh;
  block.shape.head_dim = *hd;
  if (view.size() < *sz) {
    return absl::DataLossError(
        "checkpoint payload truncated inside KV block body.");
  }
  block.payload.assign(view.data(), *sz);
  view.remove_prefix(*sz);
  // Sanity-check: the payload size must match the encoded layout for the
  // dtype/shape combination.
  const size_t expected_size = EncodedSizeBytes(block.dtype, block.shape);
  if (expected_size != *sz) {
    return absl::DataLossError(absl::StrCat(
        "KV block payload size mismatch: declared ", *sz,
        " bytes for shape (", block.shape.num_tokens, ",",
        block.shape.num_heads, ",", block.shape.head_dim,
        ") but expected ", expected_size, " bytes for the dtype."));
  }
  return block;
}

absl::Status ValidateLayerLayoutsMatch(const LayerKv& lhs, const LayerKv& rhs) {
  auto same_layout = [](const KvBlockShape& a, const KvBlockShape& b) {
    return a.num_heads == b.num_heads && a.head_dim == b.head_dim;
  };
  if (!same_layout(lhs.k.shape, rhs.k.shape) ||
      !same_layout(lhs.v.shape, rhs.v.shape)) {
    return absl::FailedPreconditionError(
        "checkpoint chain mismatch: per-layer (num_heads, head_dim) must "
        "match across the chain.");
  }
  if (lhs.k.dtype != rhs.k.dtype || lhs.v.dtype != rhs.v.dtype) {
    return absl::FailedPreconditionError(
        "checkpoint chain mismatch: per-layer KV dtype must match across "
        "the chain.");
  }
  return absl::OkStatus();
}

absl::StatusOr<EncodedKvBlock> ConcatBlocks(const EncodedKvBlock& base,
                                            const EncodedKvBlock& tail) {
  if (base.dtype != tail.dtype) {
    return absl::FailedPreconditionError(
        "ConcatBlocks: dtype mismatch.");
  }
  if (base.shape.num_heads != tail.shape.num_heads ||
      base.shape.head_dim != tail.shape.head_dim) {
    return absl::FailedPreconditionError(
        "ConcatBlocks: head layout mismatch.");
  }
  EncodedKvBlock out;
  out.dtype = base.dtype;
  out.shape.num_heads = base.shape.num_heads;
  out.shape.head_dim = base.shape.head_dim;
  out.shape.num_tokens = base.shape.num_tokens + tail.shape.num_tokens;
  // Per-token concat layout is dtype-dependent.
  switch (base.dtype) {
    case KvDtype::kFp16: {
      out.payload.reserve(base.payload.size() + tail.payload.size());
      out.payload.append(base.payload);
      out.payload.append(tail.payload);
      break;
    }
    case KvDtype::kInt8PerToken: {
      // Layout = [int8 elements...][fp16 scales...]. Concat needs to keep
      // the int8 sections together and the scale sections together.
      const size_t hd = base.shape.head_dim;
      const size_t base_groups =
          static_cast<size_t>(base.shape.num_tokens) * base.shape.num_heads;
      const size_t tail_groups =
          static_cast<size_t>(tail.shape.num_tokens) * tail.shape.num_heads;
      const size_t base_int8 = base_groups * hd;
      const size_t tail_int8 = tail_groups * hd;
      out.payload.resize(base_int8 + tail_int8 +
                         (base_groups + tail_groups) * 2);
      char* p = out.payload.data();
      // int8 section: base then tail
      std::memcpy(p, base.payload.data(), base_int8);
      p += base_int8;
      std::memcpy(p, tail.payload.data(), tail_int8);
      p += tail_int8;
      // scale section: base scales then tail scales
      std::memcpy(p, base.payload.data() + base_int8, base_groups * 2);
      p += base_groups * 2;
      std::memcpy(p, tail.payload.data() + tail_int8, tail_groups * 2);
      break;
    }
    case KvDtype::kInt4Channel:
      return absl::UnimplementedError(
          "ConcatBlocks: kInt4Channel is not implemented in Phase 2.");
  }
  return out;
}

}  // namespace

absl::StatusOr<std::string> EncodeCheckpointPayload(
    const CheckpointPayload& payload) {
  if (payload.scheme == CheckpointScheme::kDeltaSparsePages) {
    return absl::UnimplementedError(
        "kDeltaSparsePages encoding is not implemented in Phase 2.");
  }
  if (payload.layers.size() != payload.num_layers) {
    return absl::InvalidArgumentError(
        "CheckpointPayload.num_layers must equal layers.size().");
  }
  std::string out;
  out.reserve(64 + payload.layers.size() * 64);
  out.append(kPayloadMagic.data(), kPayloadMagic.size());
  AppendU8(static_cast<uint8_t>(payload.scheme), &out);
  AppendU32(payload.num_layers, &out);
  AppendU32(payload.num_tokens, &out);
  for (const LayerKv& layer : payload.layers) {
    if (auto status = EncodeBlock(layer.k, &out); !status.ok()) return status;
    if (auto status = EncodeBlock(layer.v, &out); !status.ok()) return status;
  }
  return out;
}

absl::StatusOr<CheckpointPayload> DecodeCheckpointPayload(
    absl::string_view bytes) {
  if (bytes.size() < kPayloadMagic.size() ||
      std::memcmp(bytes.data(), kPayloadMagic.data(),
                  kPayloadMagic.size()) != 0) {
    return absl::DataLossError("checkpoint payload missing magic header.");
  }
  bytes.remove_prefix(kPayloadMagic.size());

  CheckpointPayload payload;
  auto scheme_byte = ReadU8(bytes); if (!scheme_byte.ok()) return scheme_byte.status();
  switch (*scheme_byte) {
    case static_cast<uint8_t>(CheckpointScheme::kLevel0):
      payload.scheme = CheckpointScheme::kLevel0;
      break;
    case static_cast<uint8_t>(CheckpointScheme::kDeltaAppend):
      payload.scheme = CheckpointScheme::kDeltaAppend;
      break;
    case static_cast<uint8_t>(CheckpointScheme::kDeltaSparsePages):
      return absl::UnimplementedError(
          "kDeltaSparsePages decoding is not implemented in Phase 2.");
    default:
      return absl::DataLossError(
          absl::StrCat("unknown checkpoint scheme ", *scheme_byte));
  }
  auto num_layers = ReadU32(bytes); if (!num_layers.ok()) return num_layers.status();
  auto num_tokens = ReadU32(bytes); if (!num_tokens.ok()) return num_tokens.status();
  payload.num_layers = *num_layers;
  payload.num_tokens = *num_tokens;
  payload.layers.reserve(*num_layers);
  for (uint32_t i = 0; i < *num_layers; ++i) {
    LayerKv layer;
    auto k = DecodeBlock(bytes); if (!k.ok()) return k.status();
    auto v = DecodeBlock(bytes); if (!v.ok()) return v.status();
    layer.k = std::move(*k);
    layer.v = std::move(*v);
    payload.layers.push_back(std::move(layer));
  }
  if (!bytes.empty()) {
    return absl::DataLossError(absl::StrCat(
        "checkpoint payload has ", bytes.size(),
        " trailing bytes after layers."));
  }
  return payload;
}

absl::StatusOr<CheckpointPayload> BuildLevel0(uint32_t num_tokens,
                                              std::vector<LayerKv> layers) {
  CheckpointPayload p;
  p.scheme = CheckpointScheme::kLevel0;
  p.num_layers = static_cast<uint32_t>(layers.size());
  p.num_tokens = num_tokens;
  p.layers = std::move(layers);
  // Sanity: every per-layer block should declare num_tokens == p.num_tokens.
  for (size_t i = 0; i < p.layers.size(); ++i) {
    if (p.layers[i].k.shape.num_tokens != num_tokens ||
        p.layers[i].v.shape.num_tokens != num_tokens) {
      return absl::InvalidArgumentError(absl::StrCat(
          "BuildLevel0 layer ", i, " num_tokens mismatch."));
    }
  }
  return p;
}

absl::StatusOr<CheckpointPayload> BuildDeltaAppend(
    uint32_t num_appended_tokens, std::vector<LayerKv> appended_layers) {
  CheckpointPayload p;
  p.scheme = CheckpointScheme::kDeltaAppend;
  p.num_layers = static_cast<uint32_t>(appended_layers.size());
  p.num_tokens = num_appended_tokens;
  p.layers = std::move(appended_layers);
  for (size_t i = 0; i < p.layers.size(); ++i) {
    if (p.layers[i].k.shape.num_tokens != num_appended_tokens ||
        p.layers[i].v.shape.num_tokens != num_appended_tokens) {
      return absl::InvalidArgumentError(absl::StrCat(
          "BuildDeltaAppend layer ", i,
          " num_tokens mismatch with num_appended_tokens."));
    }
  }
  return p;
}

absl::StatusOr<CheckpointPayload> ReconstructFromChain(
    const std::vector<CheckpointPayload>& chain) {
  if (chain.empty()) {
    return absl::InvalidArgumentError("ReconstructFromChain on empty chain.");
  }
  if (chain.front().scheme != CheckpointScheme::kLevel0) {
    return absl::FailedPreconditionError(
        "ReconstructFromChain: first payload must be Level0.");
  }
  CheckpointPayload acc = chain.front();
  for (size_t i = 1; i < chain.size(); ++i) {
    const CheckpointPayload& delta = chain[i];
    if (delta.scheme == CheckpointScheme::kLevel0) {
      // A new Level0 in the middle of the chain is a compaction reset.
      acc = delta;
      continue;
    }
    if (delta.scheme == CheckpointScheme::kDeltaSparsePages) {
      return absl::UnimplementedError(
          "ReconstructFromChain: sparse-pages delta not implemented.");
    }
    if (delta.num_layers != acc.num_layers) {
      return absl::FailedPreconditionError(
          "ReconstructFromChain: num_layers changed mid-chain.");
    }
    for (size_t li = 0; li < delta.num_layers; ++li) {
      if (auto status =
              ValidateLayerLayoutsMatch(acc.layers[li], delta.layers[li]);
          !status.ok()) {
        return status;
      }
      auto k = ConcatBlocks(acc.layers[li].k, delta.layers[li].k);
      if (!k.ok()) return k.status();
      auto v = ConcatBlocks(acc.layers[li].v, delta.layers[li].v);
      if (!v.ok()) return v.status();
      acc.layers[li].k = std::move(*k);
      acc.layers[li].v = std::move(*v);
    }
    acc.num_tokens += delta.num_tokens;
  }
  acc.scheme = CheckpointScheme::kLevel0;  // The fully reconstructed view
                                           // is logically a Level0.
  return acc;
}

}  // namespace litert::lm
