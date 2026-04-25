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

#include "runtime/platform/hash/hasher.h"

#include <cctype>
#include <memory>
#include <string>

#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/hash/blake3_hasher.h"
#include "runtime/platform/hash/sha256_hasher.h"

namespace litert::lm {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int FromHexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::string Hash256::ToHex() const {
  std::string out;
  out.resize(64);
  for (size_t i = 0; i < 32; ++i) {
    out[i * 2] = kHexDigits[(bytes[i] >> 4) & 0xf];
    out[i * 2 + 1] = kHexDigits[bytes[i] & 0xf];
  }
  return out;
}

Hash256 Hash256::FromHex(absl::string_view hex, bool* ok) {
  Hash256 out;
  if (hex.size() != 64) {
    if (ok) *ok = false;
    return out;
  }
  for (size_t i = 0; i < 32; ++i) {
    const int hi = FromHexDigit(hex[i * 2]);
    const int lo = FromHexDigit(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      if (ok) *ok = false;
      return Hash256{};
    }
    out.bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  if (ok) *ok = true;
  return out;
}

std::unique_ptr<Hasher> CreateHasher(HashAlgorithm algo) {
  switch (algo) {
    case HashAlgorithm::kBlake3:
      return std::make_unique<Blake3Hasher>();
    case HashAlgorithm::kSha256:
      return std::make_unique<Sha256Hasher>();
  }
  // Unreachable; the enum is exhaustive.
  return std::make_unique<Blake3Hasher>();
}

Hash256 HashBytes(HashAlgorithm algo, absl::string_view data) {
  auto h = CreateHasher(algo);
  return h->OneShot(data);
}

}  // namespace litert::lm
