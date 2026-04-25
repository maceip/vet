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

#include "runtime/platform/checkpoint/local_filesystem_checkpoint_store.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gtest/gtest.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

std::filesystem::path TestRoot(absl::string_view name) {
  std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / std::string(name);
  std::filesystem::remove_all(path);
  return path;
}

TEST(LocalFilesystemCheckpointStoreTest, PutGetRoundTripBlake3) {
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_store_b3"));
  const std::string abi = "abi-bytes";
  const std::string payload = "payload-bytes-for-this-checkpoint";
  ASSERT_OK_AND_ASSIGN(Hash256 addr,
                       store.Put("tenant-a", "session-1", abi, payload,
                                 HashAlgorithm::kBlake3));
  EXPECT_EQ(addr, HashBytes(HashAlgorithm::kBlake3, payload));

  ASSERT_OK_AND_ASSIGN(auto blob, store.Get("tenant-a", "session-1", addr));
  EXPECT_EQ(blob.abi_bytes, abi);
  EXPECT_EQ(blob.payload_bytes, payload);
  EXPECT_EQ(blob.algorithm, HashAlgorithm::kBlake3);
}

TEST(LocalFilesystemCheckpointStoreTest, PutGetRoundTripSha256) {
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_store_sha"));
  const std::string abi = "abi-bytes";
  const std::string payload = "payload-bytes";
  ASSERT_OK_AND_ASSIGN(Hash256 addr,
                       store.Put("tenant-a", "session-1", abi, payload,
                                 HashAlgorithm::kSha256));
  EXPECT_EQ(addr, HashBytes(HashAlgorithm::kSha256, payload));

  ASSERT_OK_AND_ASSIGN(auto blob, store.Get("tenant-a", "session-1", addr));
  EXPECT_EQ(blob.payload_bytes, payload);
  EXPECT_EQ(blob.algorithm, HashAlgorithm::kSha256);
}

TEST(LocalFilesystemCheckpointStoreTest, IdempotentPutReturnsSameAddress) {
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_store_idem"));
  const std::string payload = "p";
  ASSERT_OK_AND_ASSIGN(Hash256 a1,
                       store.Put("tenant-a", "session-1", "abi", payload,
                                 HashAlgorithm::kBlake3));
  ASSERT_OK_AND_ASSIGN(Hash256 a2,
                       store.Put("tenant-a", "session-1", "abi", payload,
                                 HashAlgorithm::kBlake3));
  EXPECT_EQ(a1, a2);
}

TEST(LocalFilesystemCheckpointStoreTest, ExistsAndList) {
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_store_list"));
  ASSERT_OK_AND_ASSIGN(Hash256 a,
                       store.Put("tenant-a", "session-1", "x", "alpha",
                                 HashAlgorithm::kBlake3));
  ASSERT_OK_AND_ASSIGN(Hash256 b,
                       store.Put("tenant-a", "session-1", "x", "beta",
                                 HashAlgorithm::kBlake3));
  ASSERT_OK_AND_ASSIGN(bool a_exists,
                       store.Exists("tenant-a", "session-1", a));
  ASSERT_OK_AND_ASSIGN(bool b_exists,
                       store.Exists("tenant-a", "session-1", b));
  EXPECT_TRUE(a_exists);
  EXPECT_TRUE(b_exists);

  ASSERT_OK_AND_ASSIGN(std::vector<Hash256> all,
                       store.List("tenant-a", "session-1"));
  ASSERT_EQ(all.size(), 2);
  // Order is implementation-defined; just check the set.
  EXPECT_TRUE((all[0] == a && all[1] == b) ||
              (all[0] == b && all[1] == a));
}

TEST(LocalFilesystemCheckpointStoreTest,
     SecondPutWithSameAddressDifferentAbiIsRejected) {
  // Same payload bytes (so same address) but different ABI bytes — the
  // earlier "exists -> idempotent no-op" assumption was unsafe; the store
  // must now refuse rather than silently keep the old ABI.
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_store_abi_collision"));
  ASSERT_OK_AND_ASSIGN(Hash256 a,
                       store.Put("tenant-a", "session-1", "abi-A", "payload",
                                 HashAlgorithm::kBlake3));
  auto status = store.Put("tenant-a", "session-1", "abi-B", "payload",
                          HashAlgorithm::kBlake3);
  EXPECT_EQ(status.status().code(), absl::StatusCode::kDataLoss);
  ASSERT_OK_AND_ASSIGN(auto blob, store.Get("tenant-a", "session-1", a));
  EXPECT_EQ(blob.abi_bytes, "abi-A");
}

TEST(LocalFilesystemCheckpointStoreTest, GetNotFoundReturnsNotFound) {
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_store_404"));
  Hash256 fake;  // all zeros
  auto status = store.Get("tenant-a", "session-1", fake);
  EXPECT_EQ(status.status().code(), absl::StatusCode::kNotFound);
}

TEST(LocalFilesystemCheckpointStoreTest, DetectsCorruptedPayload) {
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_store_corrupt"));
  ASSERT_OK_AND_ASSIGN(Hash256 addr,
                       store.Put("tenant-a", "session-1", "abi",
                                 "original-payload",
                                 HashAlgorithm::kBlake3));
  // Corrupt the file by appending stray bytes.
  const std::filesystem::path path =
      store.PathFor("tenant-a", "session-1", addr);
  {
    std::ofstream out(path, std::ios::out | std::ios::app | std::ios::binary);
    out.put('!');
  }
  auto status = store.Get("tenant-a", "session-1", addr);
  EXPECT_EQ(status.status().code(), absl::StatusCode::kDataLoss);
}

TEST(LocalFilesystemCheckpointStoreTest, RejectsBadIdentities) {
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_store_bad"));
  EXPECT_FALSE(store.Put("..", "s", "abi", "p", HashAlgorithm::kBlake3).ok());
  EXPECT_FALSE(store.Put("t", "..", "abi", "p", HashAlgorithm::kBlake3).ok());
  EXPECT_FALSE(store.Put("ten/ant", "s", "abi", "p",
                          HashAlgorithm::kBlake3).ok());
}

}  // namespace
}  // namespace litert::lm
