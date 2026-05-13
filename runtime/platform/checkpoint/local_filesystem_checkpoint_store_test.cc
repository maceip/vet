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

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gtest/gtest.h"
#include "runtime/platform/checkpoint/canonical_manifest.h"
#include "runtime/platform/checkpoint/checkpoint_store.h"
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

Hash256 ManifestHashOf(absl::string_view label) {
  return HashBytes(HashAlgorithm::kBlake3, label);
}

CanonicalManifestInput CanonicalManifestFor(
    absl::string_view tenant_id, absl::string_view session_id,
    absl::string_view branch_id, const Hash256& body_hash,
    std::vector<Hash256> parent_hashes = {}) {
  CanonicalManifestInput input;
  input.tenant_id = std::string(tenant_id);
  input.session_id = std::string(session_id);
  input.branch_id = std::string(branch_id);
  input.parent_hashes = std::move(parent_hashes);
  input.architecture_tag = "x86_64";
  input.producer_id = "local_filesystem_checkpoint_store_test";
  input.runtime_version = "test";
  input.model_artifact_hash = ManifestHashOf("model");
  input.model_id = "test-model";
  input.schema_id = "test-schema";
  input.schema_hash = ManifestHashOf("schema");
  input.event_range_end = 1;
  input.base_event_index = 1;
  input.body_hash = body_hash;
  input.body_size_bytes = 4;
  input.created_unix_micros = 1;
  return input;
}

TEST(LocalFilesystemCheckpointStoreTest, PayloadPutGetRoundTripBlake3) {
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_payload_b3"));
  const std::string payload = "payload-bytes-for-this-checkpoint";
  ASSERT_OK_AND_ASSIGN(Hash256 body_hash,
                       store.PutPayload("tenant-a", "session-1", payload,
                                        HashAlgorithm::kBlake3));
  EXPECT_EQ(body_hash, HashBytes(HashAlgorithm::kBlake3, payload));

  ASSERT_OK_AND_ASSIGN(std::string got,
                       store.GetPayload("tenant-a", "session-1", body_hash));
  EXPECT_EQ(got, payload);
  ASSERT_OK_AND_ASSIGN(bool exists,
                       store.PayloadExists("tenant-a", "session-1", body_hash));
  EXPECT_TRUE(exists);
}

TEST(LocalFilesystemCheckpointStoreTest, IdempotentPayloadPut) {
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_payload_idem"));
  ASSERT_OK_AND_ASSIGN(Hash256 a,
                       store.PutPayload("t", "s", "p", HashAlgorithm::kBlake3));
  ASSERT_OK_AND_ASSIGN(Hash256 b,
                       store.PutPayload("t", "s", "p", HashAlgorithm::kBlake3));
  EXPECT_EQ(a, b);
}

TEST(LocalFilesystemCheckpointStoreTest, ManifestPutGetRoundTrip) {
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_manifest_rt"));
  const Hash256 body_hash = HashBytes(HashAlgorithm::kBlake3, "body");
  const Hash256 manifest_hash = ManifestHashOf("manifest-for-this-checkpoint");
  const std::string abi = "abi-bytes-go-here";
  ASSERT_OK(store.PutManifest("tenant-a", "session-1", manifest_hash, abi,
                              body_hash));
  ASSERT_OK_AND_ASSIGN(auto rec,
                       store.GetManifest("tenant-a", "session-1",
                                         manifest_hash));
  EXPECT_EQ(rec.abi_bytes, abi);
  EXPECT_EQ(rec.referenced_body_hash, body_hash);
}

TEST(LocalFilesystemCheckpointStoreTest,
     SamePayloadUnderDifferentManifestsIsLegitimate) {
  // The reason for the body/manifest split: two manifests can legitimately
  // reference the same payload bytes (e.g. branched on a different parent
  // or stamped at a different created_unix_micros).
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_shared_body"));
  const std::string payload = "kv-bytes";
  ASSERT_OK_AND_ASSIGN(Hash256 body_hash,
                       store.PutPayload("t", "s", payload,
                                        HashAlgorithm::kBlake3));

  // Same body, two different manifests.
  ASSERT_OK(store.PutManifest("t", "s", ManifestHashOf("m1"), "abi-1",
                              body_hash));
  ASSERT_OK(store.PutManifest("t", "s", ManifestHashOf("m2"), "abi-2",
                              body_hash));

  ASSERT_OK_AND_ASSIGN(auto m1,
                       store.GetManifest("t", "s", ManifestHashOf("m1")));
  ASSERT_OK_AND_ASSIGN(auto m2,
                       store.GetManifest("t", "s", ManifestHashOf("m2")));
  EXPECT_EQ(m1.abi_bytes, "abi-1");
  EXPECT_EQ(m2.abi_bytes, "abi-2");
  EXPECT_EQ(m1.referenced_body_hash, body_hash);
  EXPECT_EQ(m2.referenced_body_hash, body_hash);

  // Both manifests resolve to the same payload bytes.
  ASSERT_OK_AND_ASSIGN(std::string p,
                       store.GetPayload("t", "s", body_hash));
  EXPECT_EQ(p, payload);
}

TEST(LocalFilesystemCheckpointStoreTest, DifferentManifestBytesAtSameAddressRejected) {
  // Same manifest_hash with different ABI bytes — that's a real collision
  // and must not be silently accepted.
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_manifest_collision"));
  const Hash256 mh = ManifestHashOf("m");
  const Hash256 bh = HashBytes(HashAlgorithm::kBlake3, "p");
  ASSERT_OK(store.PutManifest("t", "s", mh, "abi-A", bh));
  auto status = store.PutManifest("t", "s", mh, "abi-B", bh);
  EXPECT_EQ(status.code(), absl::StatusCode::kDataLoss);
}

TEST(LocalFilesystemCheckpointStoreTest, ListManifestsReturnsBoth) {
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_list"));
  const Hash256 bh = HashBytes(HashAlgorithm::kBlake3, "p");
  const Hash256 m1 = ManifestHashOf("m1");
  const Hash256 m2 = ManifestHashOf("m2");
  ASSERT_OK(store.PutManifest("t", "s", m1, "abi-1", bh));
  ASSERT_OK(store.PutManifest("t", "s", m2, "abi-2", bh));
  ASSERT_OK_AND_ASSIGN(std::vector<Hash256> all,
                       store.ListManifests("t", "s"));
  ASSERT_EQ(all.size(), 2);
  EXPECT_TRUE((all[0] == m1 && all[1] == m2) ||
              (all[0] == m2 && all[1] == m1));
}

TEST(LocalFilesystemCheckpointStoreTest, ListsDependentManifestsByParent) {
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_dependents"));
  const Hash256 parent_body = HashBytes(HashAlgorithm::kBlake3, "body");
  CanonicalManifestInput parent =
      CanonicalManifestFor("t", "s", "main", parent_body);
  ASSERT_OK_AND_ASSIGN(std::string parent_abi,
                       EncodeCanonicalManifest(parent));
  ASSERT_OK_AND_ASSIGN(Hash256 parent_hash,
                       ComputeManifestHash(HashAlgorithm::kBlake3, parent));
  ASSERT_OK(store.PutManifest("t", "s", parent_hash, parent_abi,
                              parent_body));

  const Hash256 child_body = HashBytes(HashAlgorithm::kBlake3, "body2");
  CanonicalManifestInput child =
      CanonicalManifestFor("t", "s", "main", child_body, {parent_hash});
  child.event_range_start = 1;
  child.event_range_end = 2;
  child.base_event_index = 2;
  ASSERT_OK_AND_ASSIGN(std::string child_abi,
                       EncodeCanonicalManifest(child));
  ASSERT_OK_AND_ASSIGN(Hash256 child_hash,
                       ComputeManifestHash(HashAlgorithm::kBlake3, child));
  ASSERT_OK(store.PutManifest("t", "s", child_hash, child_abi, child_body));

  ASSERT_OK_AND_ASSIGN(std::vector<Hash256> dependents,
                       store.ListDependentManifests("t", "s", parent_hash));
  EXPECT_EQ(dependents.size(), 1);
  EXPECT_NE(std::find(dependents.begin(), dependents.end(), child_hash),
            dependents.end());
}

TEST(LocalFilesystemCheckpointStoreTest, GetMissingPayloadIsNotFound) {
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_404_payload"));
  Hash256 fake;
  EXPECT_EQ(store.GetPayload("t", "s", fake).status().code(),
            absl::StatusCode::kNotFound);
}

TEST(LocalFilesystemCheckpointStoreTest, GetMissingManifestIsNotFound) {
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_404_manifest"));
  Hash256 fake;
  EXPECT_EQ(store.GetManifest("t", "s", fake).status().code(),
            absl::StatusCode::kNotFound);
}

TEST(LocalFilesystemCheckpointStoreTest, DetectsCorruptedPayload) {
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_corrupt_payload"));
  ASSERT_OK_AND_ASSIGN(Hash256 body_hash,
                       store.PutPayload("t", "s", "original",
                                        HashAlgorithm::kBlake3));
  const std::filesystem::path path =
      store.PayloadPathFor("t", "s", body_hash);
  {
    std::ofstream out(path, std::ios::out | std::ios::app | std::ios::binary);
    out.put('!');
  }
  EXPECT_EQ(store.GetPayload("t", "s", body_hash).status().code(),
            absl::StatusCode::kDataLoss);
}

TEST(LocalFilesystemCheckpointStoreTest, RejectsBadIdentities) {
  LocalFilesystemCheckpointStore store(TestRoot("ckpt_bad_id"));
  EXPECT_FALSE(store.PutPayload("..", "s", "p", HashAlgorithm::kBlake3).ok());
  EXPECT_FALSE(store.PutPayload("t", "..", "p", HashAlgorithm::kBlake3).ok());
  Hash256 fake;
  EXPECT_FALSE(store.PutManifest("..", "s", fake, "abi", fake).ok());
}

}  // namespace
}  // namespace litert::lm
