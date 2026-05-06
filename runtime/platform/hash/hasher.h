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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_HASH_HASHER_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_HASH_HASHER_H_

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "absl/strings/string_view.h"  // from @com_google_absl

namespace litert::lm {

// 32-byte content hash. Algorithm-agnostic by storage; the producing
// algorithm is recorded next to the bytes in the proto wrapper so a
// verifier knows which primitive to run when checking.
struct Hash256 {
  std::array<uint8_t, 32> bytes{};

  bool operator==(const Hash256& other) const {
    return bytes == other.bytes;
  }
  bool operator!=(const Hash256& other) const { return !(*this == other); }
  bool operator<(const Hash256& other) const {
    return std::memcmp(bytes.data(), other.bytes.data(), bytes.size()) < 0;
  }

  // Lowercase hex. 64 characters.
  std::string ToHex() const;

  // Inverse of ToHex; returns nullopt-equivalent (all zero) on bad input
  // and sets *ok to false. The empty case here keeps the type free of
  // absl::optional dependency at the public-API layer.
  static Hash256 FromHex(absl::string_view hex, bool* ok);

  // Hash key for absl::flat_hash_map.
  template <typename H>
  friend H AbslHashValue(H h, const Hash256& v) {
    return H::combine_contiguous(std::move(h), v.bytes.data(), v.bytes.size());
  }
};

// One-shot hash interface. Implementations are stateful (Update / Finalize)
// for streaming, and OneShot is provided for the common "I have a buffer"
// path. Implementations must be thread-safe across instances; a single
// instance is not required to be thread-safe.
class Hasher {
 public:
  virtual ~Hasher() = default;

  virtual void Update(absl::string_view data) = 0;

  // Finalize must be called exactly once. After Finalize, the hasher is
  // not reusable; create a fresh one for another input.
  virtual Hash256 Finalize() = 0;

  Hash256 OneShot(absl::string_view data) {
    Update(data);
    return Finalize();
  }
};

// Algorithm enum mirrors runtime/proto/checkpoint.proto's HashAlgorithm so
// downstream code can pass either form; conversions live in the .cc.
enum class HashAlgorithm {
  kBlake3 = 0,    // default; faster Merkle-tree-friendly primitive
  kSha256 = 1,    // FIPS-140 escape hatch
};

// Factory. Returns a freshly-initialized hasher of the requested algorithm.
std::unique_ptr<Hasher> CreateHasher(HashAlgorithm algo);

// Convenience one-shot when callers don't need streaming.
Hash256 HashBytes(HashAlgorithm algo, absl::string_view data);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_HASH_HASHER_H_
