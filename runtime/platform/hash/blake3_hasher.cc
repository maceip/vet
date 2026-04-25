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

// BLAKE3 single-threaded reference implementation in pure C++.
// Spec: https://github.com/BLAKE3-team/BLAKE3-specs (commit 2020-01-09).
// Test vectors: hash of N zero bytes for N in {0, 1, 63, 64, 65, 1023,
// 1024, 1025, 2048, 2049, 3072, 3073, 4096, 4097, 5120, 5121, 6144, 6145,
// 7168, 7169, 8192, 8193, 16384, 31744, 102400}. Each first 32 bytes
// matches the reference output.

#include "runtime/platform/hash/blake3_hasher.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "absl/strings/string_view.h"  // from @com_google_absl

namespace litert::lm {
namespace {

constexpr uint32_t kIv[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

constexpr uint32_t kFlagChunkStart = 1u << 0;
constexpr uint32_t kFlagChunkEnd = 1u << 1;
constexpr uint32_t kFlagParent = 1u << 2;
constexpr uint32_t kFlagRoot = 1u << 3;

constexpr int kBlockLen = 64;
constexpr int kChunkLen = 1024;

// Message word permutation per round.
constexpr uint8_t kMsgPermutation[16] = {
    2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8,
};

inline uint32_t Rotr(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

inline void G(std::array<uint32_t, 16>& s, int a, int b, int c, int d,
              uint32_t mx, uint32_t my) {
  s[a] = s[a] + s[b] + mx;
  s[d] = Rotr(s[d] ^ s[a], 16);
  s[c] = s[c] + s[d];
  s[b] = Rotr(s[b] ^ s[c], 12);
  s[a] = s[a] + s[b] + my;
  s[d] = Rotr(s[d] ^ s[a], 8);
  s[c] = s[c] + s[d];
  s[b] = Rotr(s[b] ^ s[c], 7);
}

inline void Round(std::array<uint32_t, 16>& s,
                  const std::array<uint32_t, 16>& m) {
  // Mix the columns.
  G(s, 0, 4, 8, 12, m[0], m[1]);
  G(s, 1, 5, 9, 13, m[2], m[3]);
  G(s, 2, 6, 10, 14, m[4], m[5]);
  G(s, 3, 7, 11, 15, m[6], m[7]);
  // Mix the diagonals.
  G(s, 0, 5, 10, 15, m[8], m[9]);
  G(s, 1, 6, 11, 12, m[10], m[11]);
  G(s, 2, 7, 8, 13, m[12], m[13]);
  G(s, 3, 4, 9, 14, m[14], m[15]);
}

inline void Permute(std::array<uint32_t, 16>& m) {
  std::array<uint32_t, 16> permuted;
  for (int i = 0; i < 16; ++i) permuted[i] = m[kMsgPermutation[i]];
  m = permuted;
}

// The BLAKE3 compression function. cv = 8 32-bit chaining input;
// block = 64 raw bytes; counter = chunk index for chunk nodes (0 for
// parent nodes); block_len = bytes used in this block; flags = flag bits.
// Returns the full 16-word output state (the first 8 are the new CV, the
// remaining 8 are used at the root for the extended output).
std::array<uint32_t, 16> Compress(const std::array<uint32_t, 8>& cv,
                                  const uint8_t block[kBlockLen],
                                  uint64_t counter, uint32_t block_len,
                                  uint32_t flags) {
  std::array<uint32_t, 16> m;
  for (int i = 0; i < 16; ++i) {
    m[i] =
        static_cast<uint32_t>(block[i * 4]) |
        (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
        (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
        (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
  }
  std::array<uint32_t, 16> s = {
      cv[0], cv[1], cv[2], cv[3],
      cv[4], cv[5], cv[6], cv[7],
      kIv[0], kIv[1], kIv[2], kIv[3],
      static_cast<uint32_t>(counter & 0xFFFFFFFFu),
      static_cast<uint32_t>(counter >> 32),
      block_len,
      flags,
  };
  Round(s, m);
  for (int r = 0; r < 6; ++r) {
    Permute(m);
    Round(s, m);
  }
  // First half mixed with second half to produce final output state.
  for (int i = 0; i < 8; ++i) {
    s[i] = s[i] ^ s[i + 8];
    s[i + 8] = s[i + 8] ^ cv[i];
  }
  return s;
}

std::array<uint32_t, 8> ChainingValue(const std::array<uint32_t, 16>& out) {
  std::array<uint32_t, 8> cv;
  for (int i = 0; i < 8; ++i) cv[i] = out[i];
  return cv;
}

std::array<uint32_t, 8> ParentChainingValue(
    const std::array<uint32_t, 8>& left,
    const std::array<uint32_t, 8>& right,
    const std::array<uint32_t, 8>& key, uint32_t flags) {
  uint8_t block[kBlockLen];
  for (int i = 0; i < 8; ++i) {
    block[i * 4] = static_cast<uint8_t>(left[i]);
    block[i * 4 + 1] = static_cast<uint8_t>(left[i] >> 8);
    block[i * 4 + 2] = static_cast<uint8_t>(left[i] >> 16);
    block[i * 4 + 3] = static_cast<uint8_t>(left[i] >> 24);
  }
  for (int i = 0; i < 8; ++i) {
    block[32 + i * 4] = static_cast<uint8_t>(right[i]);
    block[32 + i * 4 + 1] = static_cast<uint8_t>(right[i] >> 8);
    block[32 + i * 4 + 2] = static_cast<uint8_t>(right[i] >> 16);
    block[32 + i * 4 + 3] = static_cast<uint8_t>(right[i] >> 24);
  }
  return ChainingValue(
      Compress(key, block, /*counter=*/0, kBlockLen, flags | kFlagParent));
}

}  // namespace

void Blake3Hasher::ChunkState::Reset(const std::array<uint32_t, 8>& key,
                                     uint64_t counter) {
  cv = key;
  block.fill(0);
  block_len = 0;
  blocks_compressed = 0;
  chunk_counter = counter;
}

void Blake3Hasher::ChunkState::Update(const uint8_t* input, size_t len) {
  while (len > 0) {
    if (block_len == kBlockLen) {
      // Compress the full block (not the last block of this chunk).
      uint32_t flags = 0;
      if (blocks_compressed == 0) flags |= kFlagChunkStart;
      auto out = Compress(cv, block.data(), chunk_counter, kBlockLen, flags);
      cv = ChainingValue(out);
      ++blocks_compressed;
      block_len = 0;
    }
    const size_t want = kBlockLen - block_len;
    const size_t take = std::min(want, len);
    std::memcpy(block.data() + block_len, input, take);
    block_len = static_cast<uint8_t>(block_len + take);
    input += take;
    len -= take;
  }
}

std::array<uint32_t, 16> Blake3Hasher::ChunkState::Output(uint32_t flags) const {
  uint32_t f = flags;
  if (blocks_compressed == 0) f |= kFlagChunkStart;
  f |= kFlagChunkEnd;
  return Compress(cv, block.data(), chunk_counter, block_len, f);
}

Blake3Hasher::Blake3Hasher() {
  for (int i = 0; i < 8; ++i) key_[i] = kIv[i];
  chunk_.Reset(key_, /*counter=*/0);
  cv_stack_.resize(54);  // BLAKE3 supports up to 2^54 chunks.
}

void Blake3Hasher::AddChunkChainingValue(const std::array<uint32_t, 8>& cv) {
  // Merge full subtrees: for every set bit in the (chunk_index + 1)
  // counter that is now "filled in" to the next power of two, pop a
  // sibling and replace with the parent CV.
  uint64_t total_chunks = chunk_.chunk_counter + 1;
  std::array<uint32_t, 8> new_cv = cv;
  while ((total_chunks & 1) == 0) {
    new_cv = ParentChainingValue(cv_stack_[cv_stack_len_ - 1], new_cv, key_,
                                 flags_);
    --cv_stack_len_;
    total_chunks >>= 1;
  }
  cv_stack_[cv_stack_len_++] = new_cv;
}

void Blake3Hasher::Update(absl::string_view data) {
  if (finalized_) return;
  const uint8_t* input = reinterpret_cast<const uint8_t*>(data.data());
  size_t len = data.size();
  while (len > 0) {
    if (chunk_.block_len == kBlockLen &&
        chunk_.blocks_compressed * kBlockLen + kBlockLen == kChunkLen) {
      // The current chunk's last block is filled. Finalize as a chunk
      // chaining value and start the next chunk.
      auto out = chunk_.Output(flags_);
      AddChunkChainingValue(ChainingValue(out));
      chunk_.Reset(key_, chunk_.chunk_counter + 1);
    }
    const size_t chunk_remaining =
        kChunkLen - (chunk_.blocks_compressed * kBlockLen + chunk_.block_len);
    const size_t take = std::min(chunk_remaining, len);
    chunk_.Update(input, take);
    input += take;
    len -= take;
  }
}

Hash256 Blake3Hasher::Finalize() {
  Hash256 digest;
  if (finalized_) return digest;
  finalized_ = true;

  // Walk up the cv_stack_ merging right-to-left with the current chunk's
  // output, applying ROOT only at the very top.
  if (cv_stack_len_ == 0) {
    // The single-chunk case: finalize the chunk with the ROOT flag.
    auto out = chunk_.Output(flags_ | kFlagRoot);
    for (int i = 0; i < 8; ++i) {
      digest.bytes[i * 4] = static_cast<uint8_t>(out[i]);
      digest.bytes[i * 4 + 1] = static_cast<uint8_t>(out[i] >> 8);
      digest.bytes[i * 4 + 2] = static_cast<uint8_t>(out[i] >> 16);
      digest.bytes[i * 4 + 3] = static_cast<uint8_t>(out[i] >> 24);
    }
    return digest;
  }

  // Multi-chunk case: collapse the stack with the current chunk's output.
  auto chunk_out = chunk_.Output(flags_);
  std::array<uint32_t, 8> right_cv = ChainingValue(chunk_out);
  // The last (topmost) merge needs the ROOT flag, and uses the full output
  // expansion. We have to identify which step is the root; it's the final
  // merge of stack[0] with the running right_cv.
  while (cv_stack_len_ > 1) {
    right_cv = ParentChainingValue(cv_stack_[cv_stack_len_ - 1], right_cv,
                                   key_, flags_);
    --cv_stack_len_;
  }
  // Final root parent compression.
  uint8_t root_block[kBlockLen];
  for (int i = 0; i < 8; ++i) {
    root_block[i * 4] = static_cast<uint8_t>(cv_stack_[0][i]);
    root_block[i * 4 + 1] = static_cast<uint8_t>(cv_stack_[0][i] >> 8);
    root_block[i * 4 + 2] = static_cast<uint8_t>(cv_stack_[0][i] >> 16);
    root_block[i * 4 + 3] = static_cast<uint8_t>(cv_stack_[0][i] >> 24);
  }
  for (int i = 0; i < 8; ++i) {
    root_block[32 + i * 4] = static_cast<uint8_t>(right_cv[i]);
    root_block[32 + i * 4 + 1] = static_cast<uint8_t>(right_cv[i] >> 8);
    root_block[32 + i * 4 + 2] = static_cast<uint8_t>(right_cv[i] >> 16);
    root_block[32 + i * 4 + 3] = static_cast<uint8_t>(right_cv[i] >> 24);
  }
  auto root_out = Compress(key_, root_block, /*counter=*/0, kBlockLen,
                           flags_ | kFlagParent | kFlagRoot);
  for (int i = 0; i < 8; ++i) {
    digest.bytes[i * 4] = static_cast<uint8_t>(root_out[i]);
    digest.bytes[i * 4 + 1] = static_cast<uint8_t>(root_out[i] >> 8);
    digest.bytes[i * 4 + 2] = static_cast<uint8_t>(root_out[i] >> 16);
    digest.bytes[i * 4 + 3] = static_cast<uint8_t>(root_out[i] >> 24);
  }
  return digest;
}

}  // namespace litert::lm
