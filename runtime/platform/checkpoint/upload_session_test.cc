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

CheckpointUploadSession::ManifestMeta MakeMeta(absl::string_view payload,
                                               absl::string_view abi,
                                               absl::string_view manifest_label) {
  CheckpointUploadSession::ManifestMeta m;
  m.declared_payload_size_bytes = payload.size();
  m.expected_body_hash = HashBytes(HashAlgorithm::kBlake3, payload);
  m.algo = HashAlgorithm::kBlake3;
  m.abi_bytes.assign(abi.data(), abi.size());
  m.manifest_hash = HashBytes(HashAlgorithm::kBlake3, manifest_label);
  return m;
}

TEST(CheckpointUploadSessionTest, RoundTripFinalizesViaStore) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_round_trip"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);

  const std::string abi = "abi-bytes";
  const std::string frame_a = "first-half";
  const std::string frame_b = "second-half";
  const std::string full = frame_a + frame_b;
  auto meta = MakeMeta(full, abi, "m1");

  ASSERT_OK(s.BeginManifest(meta));
  ASSERT_OK(s.AddFrame(0, frame_a));
  ASSERT_OK(s.AddFrame(frame_a.size(), frame_b));
  ASSERT_OK_AND_ASSIGN(Hash256 manifest_hash, s.Finalize());
  EXPECT_EQ(manifest_hash, meta.manifest_hash);
  EXPECT_TRUE(s.finalized());

  // Both halves of the split address space are populated.
  ASSERT_OK_AND_ASSIGN(std::string payload,
                       store.GetPayload("tenant-a", "session-1",
                                        meta.expected_body_hash));
  EXPECT_EQ(payload, full);
  ASSERT_OK_AND_ASSIGN(auto rec,
                       store.GetManifest("tenant-a", "session-1",
                                         meta.manifest_hash));
  EXPECT_EQ(rec.abi_bytes, abi);
  EXPECT_EQ(rec.referenced_body_hash, meta.expected_body_hash);
}

TEST(CheckpointUploadSessionTest, FinalizeRejectsBodyHashMismatch) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_body_mismatch"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);

  const std::string declared = "promised-bytes";
  const std::string actual_streamed = "different-bytes";
  auto meta = MakeMeta(declared, "abi", "m");
  meta.declared_payload_size_bytes = actual_streamed.size();

  ASSERT_OK(s.BeginManifest(meta));
  ASSERT_OK(s.AddFrame(0, actual_streamed));
  auto fin = s.Finalize();
  EXPECT_EQ(fin.status().code(), absl::StatusCode::kDataLoss);
  EXPECT_FALSE(s.finalized());

  // Crucially, the store must not contain either the payload or the
  // manifest after a refused commit.
  ASSERT_OK_AND_ASSIGN(bool payload_exists,
                       store.PayloadExists("tenant-a", "session-1",
                                           meta.expected_body_hash));
  ASSERT_OK_AND_ASSIGN(bool manifest_exists,
                       store.ManifestExists("tenant-a", "session-1",
                                            meta.manifest_hash));
  EXPECT_FALSE(payload_exists);
  EXPECT_FALSE(manifest_exists);
}

TEST(CheckpointUploadSessionTest, RejectsZeroExpectedBodyHash) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_zero_body_hash"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);
  auto meta = MakeMeta("p", "abi", "m");
  Hash256 zero;
  meta.expected_body_hash = zero;
  EXPECT_FALSE(s.BeginManifest(meta).ok());
}

TEST(CheckpointUploadSessionTest, RejectsZeroManifestHash) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_zero_manifest_hash"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);
  auto meta = MakeMeta("p", "abi", "m");
  Hash256 zero;
  meta.manifest_hash = zero;
  EXPECT_FALSE(s.BeginManifest(meta).ok());
}

TEST(CheckpointUploadSessionTest, RejectsAddFrameBeforeBegin) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_no_begin"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);
  EXPECT_FALSE(s.AddFrame(0, "x").ok());
}

TEST(CheckpointUploadSessionTest, RejectsOutOfOrderFrame) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_oob"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);
  ASSERT_OK(s.BeginManifest(MakeMeta("0123456789", "abi", "m")));
  ASSERT_OK(s.AddFrame(0, "abcde"));
  EXPECT_FALSE(s.AddFrame(0, "fghij").ok());
  EXPECT_FALSE(s.AddFrame(99, "fghij").ok());
}

TEST(CheckpointUploadSessionTest, RejectsFrameExceedingDeclaredSize) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_too_big"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);
  auto meta = MakeMeta("abcd", "abi", "m");
  ASSERT_OK(s.BeginManifest(meta));
  EXPECT_FALSE(s.AddFrame(0, "five!").ok());
}

TEST(CheckpointUploadSessionTest, RejectsFinalizeWithMissingBytes) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_short"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);
  auto meta = MakeMeta("0123456789", "abi", "m");
  ASSERT_OK(s.BeginManifest(meta));
  ASSERT_OK(s.AddFrame(0, "01234"));
  auto fin = s.Finalize();
  EXPECT_EQ(fin.status().code(), absl::StatusCode::kDataLoss);
  EXPECT_FALSE(s.finalized());
}

TEST(CheckpointUploadSessionTest, RejectsBeginTwice) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_re_begin"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);
  ASSERT_OK(s.BeginManifest(MakeMeta("p", "abi", "m")));
  EXPECT_FALSE(s.BeginManifest(MakeMeta("p", "abi", "m")).ok());
}

TEST(CheckpointUploadSessionTest, FinalizeWithSha256AlsoVerifies) {
  LocalFilesystemCheckpointStore store(TestRoot("upload_sha"));
  CheckpointUploadSession s("tenant-a", "session-1", &store);
  const std::string payload = "payload";
  CheckpointUploadSession::ManifestMeta meta;
  meta.declared_payload_size_bytes = payload.size();
  meta.expected_body_hash = HashBytes(HashAlgorithm::kSha256, payload);
  meta.algo = HashAlgorithm::kSha256;
  meta.abi_bytes = "abi";
  meta.manifest_hash = HashBytes(HashAlgorithm::kSha256, "m");
  ASSERT_OK(s.BeginManifest(meta));
  ASSERT_OK(s.AddFrame(0, payload));
  ASSERT_OK_AND_ASSIGN(Hash256 manifest_hash, s.Finalize());
  EXPECT_EQ(manifest_hash, meta.manifest_hash);
}

}  // namespace
}  // namespace litert::lm
