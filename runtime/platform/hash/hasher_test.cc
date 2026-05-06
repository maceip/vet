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

#include <cstdint>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gtest/gtest.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

// SHA-256 reference test vectors (FIPS 180-4 / NIST CAVS).
TEST(Sha256Test, EmptyInputMatchesReferenceVector) {
  // SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
  EXPECT_EQ(HashBytes(HashAlgorithm::kSha256, "").ToHex(),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256Test, AbcMatchesReferenceVector) {
  EXPECT_EQ(HashBytes(HashAlgorithm::kSha256, "abc").ToHex(),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256Test, FiftySixCharFipsVector) {
  // FIPS 180-4 published vector: SHA-256 of "abcdbcde...kjlmnopnopq" (the
  // 56-byte ASCII string used as the second canonical test).
  EXPECT_EQ(
      HashBytes(HashAlgorithm::kSha256,
                "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
          .ToHex(),
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256Test, OneMillionA) {
  // FIPS 180-4 published vector.
  std::string million_a(1'000'000, 'a');
  EXPECT_EQ(HashBytes(HashAlgorithm::kSha256, million_a).ToHex(),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256Test, StreamingEqualsOneShot) {
  auto h = CreateHasher(HashAlgorithm::kSha256);
  h->Update("abc");
  h->Update("def");
  Hash256 streamed = h->Finalize();
  EXPECT_EQ(streamed, HashBytes(HashAlgorithm::kSha256, "abcdef"));
}

// BLAKE3 reference test vectors. The official BLAKE3 test vector format
// pumps the byte pattern (i mod 251) into the hasher; the expected
// outputs are pinned in the project's test_vectors.json.
std::string Blake3InputBytes(size_t len) {
  std::string out;
  out.resize(len);
  for (size_t i = 0; i < len; ++i) {
    out[i] = static_cast<char>(i % 251);
  }
  return out;
}

TEST(Blake3Test, EmptyInputMatchesReferenceVector) {
  EXPECT_EQ(HashBytes(HashAlgorithm::kBlake3, "").ToHex(),
            "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
}

TEST(Blake3Test, OneByteMatchesReferenceVector) {
  EXPECT_EQ(HashBytes(HashAlgorithm::kBlake3, std::string("\x00", 1)).ToHex(),
            "2d3adedff11b61f14c886e35afa036014d04f9f5b779aaab057c7d4f8c45ed94");
}

TEST(Blake3Test, ChunkBoundaryAt1023Bytes) {
  // Length 1023: one byte short of a full chunk; exercises the
  // "single chunk, last block partial" path.
  std::string input = Blake3InputBytes(1023);
  EXPECT_EQ(HashBytes(HashAlgorithm::kBlake3, input).ToHex(),
            "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11");
}

TEST(Blake3Test, ExactlyOneChunk) {
  // Length 1024: exactly fills a chunk; exercises "single chunk, last
  // block exactly full" path (no CHUNK_START on the last block).
  std::string input = Blake3InputBytes(1024);
  EXPECT_EQ(HashBytes(HashAlgorithm::kBlake3, input).ToHex(),
            "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7");
}

TEST(Blake3Test, JustOverOneChunk) {
  // Length 1025: triggers the multi-chunk merge path with a tiny second
  // chunk; exercises ParentChainingValue + ROOT-on-parent semantics.
  std::string input = Blake3InputBytes(1025);
  EXPECT_EQ(HashBytes(HashAlgorithm::kBlake3, input).ToHex(),
            "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444");
}

TEST(Blake3Test, EightChunksEvenSubtree) {
  // Length 8192: exactly 8 chunks; exercises a balanced subtree merge.
  std::string input = Blake3InputBytes(8192);
  EXPECT_EQ(HashBytes(HashAlgorithm::kBlake3, input).ToHex(),
            "5cf7e3da27c4ec3416639bdf190b8c8a17c0a8c10ade5237fd1f78fbe79c2080");
}

TEST(Blake3Test, StreamingEqualsOneShot) {
  std::string input = Blake3InputBytes(2 * 1024 + 17);
  Hash256 one_shot = HashBytes(HashAlgorithm::kBlake3, input);
  auto h = CreateHasher(HashAlgorithm::kBlake3);
  // Stream in oddly-sized chunks to stress the buffering path.
  h->Update(absl::string_view(input.data(), 5));
  h->Update(absl::string_view(input.data() + 5, 1024 + 1));
  h->Update(absl::string_view(input.data() + 5 + 1025, input.size() - 5 - 1025));
  EXPECT_EQ(h->Finalize(), one_shot);
}

TEST(Hash256Test, HexRoundTrips) {
  bool ok = false;
  const Hash256 a = HashBytes(HashAlgorithm::kSha256, "abc");
  const std::string hex = a.ToHex();
  EXPECT_EQ(hex.size(), 64);
  Hash256 b = Hash256::FromHex(hex, &ok);
  EXPECT_TRUE(ok);
  EXPECT_EQ(a, b);
}

TEST(Hash256Test, FromHexRejectsBadLengthAndBadChars) {
  bool ok = true;
  Hash256::FromHex("short", &ok);
  EXPECT_FALSE(ok);
  ok = true;
  Hash256::FromHex(std::string(64, 'z'), &ok);
  EXPECT_FALSE(ok);
}

TEST(Hash256Test, ComparisonAndOrder) {
  Hash256 a = HashBytes(HashAlgorithm::kSha256, "a");
  Hash256 b = HashBytes(HashAlgorithm::kSha256, "b");
  EXPECT_NE(a, b);
  EXPECT_TRUE(a < b || b < a);
}

}  // namespace
}  // namespace litert::lm
