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

#include "runtime/platform/checkpoint/upload_session.h"

#include <filesystem>
#include <string>

#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gtest/gtest.h"
#include "runtime/platform/checkpoint/local_filesystem_checkpoint_store.h"
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

TEST(CheckpointUploadSessionTest, RoundTripFinalizesViaStore) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_session_round_trip"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);

  const std::string abi = "abi-bytes";
  const std::string frame_a = "first-half";
  const std::string frame_b = "second-half";
  const std::string full = frame_a + frame_b;

  ASSERT_OK(s.BeginManifest(full.size(), abi, HashAlgorithm::kBlake3));
  ASSERT_OK(s.AddFrame(0, frame_a));
  ASSERT_OK(s.AddFrame(frame_a.size(), frame_b));
  ASSERT_OK_AND_ASSIGN(Hash256 addr, s.Finalize());
  EXPECT_TRUE(s.finalized());

  ASSERT_OK_AND_ASSIGN(auto blob, store.Get("tenant-a", "session-1", addr));
  EXPECT_EQ(blob.abi_bytes, abi);
  EXPECT_EQ(blob.payload_bytes, full);
  EXPECT_EQ(addr, HashBytes(HashAlgorithm::kBlake3, full));
}

TEST(CheckpointUploadSessionTest, RejectsAddFrameBeforeBegin) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_session_no_begin"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);
  EXPECT_FALSE(s.AddFrame(0, "x").ok());
}

TEST(CheckpointUploadSessionTest, RejectsOutOfOrderFrame) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_session_oob"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);
  ASSERT_OK(s.BeginManifest(10, "abi", HashAlgorithm::kBlake3));
  ASSERT_OK(s.AddFrame(0, "abcde"));
  EXPECT_FALSE(s.AddFrame(0, "fghij").ok());      // overlap
  EXPECT_FALSE(s.AddFrame(99, "fghij").ok());     // gap
}

TEST(CheckpointUploadSessionTest, RejectsFrameExceedingDeclaredSize) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_session_too_big"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);
  ASSERT_OK(s.BeginManifest(4, "abi", HashAlgorithm::kBlake3));
  EXPECT_FALSE(s.AddFrame(0, "five!").ok());
}

TEST(CheckpointUploadSessionTest, RejectsFinalizeWithMissingBytes) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_session_short"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);
  ASSERT_OK(s.BeginManifest(10, "abi", HashAlgorithm::kBlake3));
  ASSERT_OK(s.AddFrame(0, "abcde"));
  auto fin = s.Finalize();
  EXPECT_EQ(fin.status().code(), absl::StatusCode::kDataLoss);
  EXPECT_FALSE(s.finalized());
}

TEST(CheckpointUploadSessionTest, RejectsBeginTwice) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_session_re_begin"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);
  ASSERT_OK(s.BeginManifest(10, "abi", HashAlgorithm::kBlake3));
  EXPECT_FALSE(s.BeginManifest(10, "abi", HashAlgorithm::kBlake3).ok());
}

TEST(CheckpointUploadSessionTest, ZeroDeclaredSizeRejected) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_session_zero"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);
  EXPECT_FALSE(s.BeginManifest(0, "abi", HashAlgorithm::kBlake3).ok());
}

TEST(CheckpointUploadSessionTest, FinalizeWithSha256AlsoWorks) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_session_sha"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);
  const std::string payload = "payload";
  ASSERT_OK(s.BeginManifest(payload.size(), "abi", HashAlgorithm::kSha256));
  ASSERT_OK(s.AddFrame(0, payload));
  ASSERT_OK_AND_ASSIGN(Hash256 addr, s.Finalize());
  EXPECT_EQ(addr, HashBytes(HashAlgorithm::kSha256, payload));
}

}  // namespace
}  // namespace litert::lm
