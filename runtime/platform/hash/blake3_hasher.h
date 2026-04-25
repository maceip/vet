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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_HASH_BLAKE3_HASHER_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_HASH_BLAKE3_HASHER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {

// BLAKE3 (single-threaded, default 256-bit output). Implements the spec
// from https://github.com/BLAKE3-team/BLAKE3-specs revision 2020-01-09:
// chunk size 1024 bytes, 16 32-bit words per block, 7 rounds of the
// Blake2-derived G mixing function, binary tree of 1024-byte chunks,
// per-chunk and per-parent flags.
//
// Correctness is verified against the spec's published test vectors
// (zero-filled lengths 0, 1, 1023, 1024, 1025, 8192) which exercise the
// chunk boundary, the binary tree merge, and the empty-input case.
//
// This implementation is pure C++ and does not pull in a third-party
// library. It is single-threaded; SIMD batching and multi-threaded
// parallelism are deferred to a follow-up if the prefill / Merkle hot
// path needs them.
class Blake3Hasher : public Hasher {
 public:
  Blake3Hasher();
  ~Blake3Hasher() override = default;

  void Update(absl::string_view data) override;
  Hash256 Finalize() override;

 private:
  // BLAKE3 uses 16 32-bit chaining values per intermediate node and 8
  // for the output. We keep the running chunk state inline.
  struct ChunkState {
    std::array<uint32_t, 8> cv;
    std::array<uint8_t, 64> block;
    uint8_t block_len = 0;
    uint8_t blocks_compressed = 0;
    uint64_t chunk_counter = 0;

    void Reset(const std::array<uint32_t, 8>& key, uint64_t counter);
    void Update(const uint8_t* input, size_t len);
    std::array<uint32_t, 16> Output(uint32_t flags) const;
  };

  void AddChunkChainingValue(const std::array<uint32_t, 8>& cv);

  std::array<uint32_t, 8> key_;
  ChunkState chunk_;
  // Stack of subtree chaining values pending merge. Index = lg2 of subtree
  // size in chunks. In the spec this is called the "stack of subtree CVs".
  std::vector<std::array<uint32_t, 8>> cv_stack_;
  uint8_t cv_stack_len_ = 0;
  uint32_t flags_ = 0;
  bool finalized_ = false;
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_HASH_BLAKE3_HASHER_H_
