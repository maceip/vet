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

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gtest/gtest.h"
#include "runtime/dpm/projection_prompt.h"
#include "runtime/platform/checkpoint/canonical_manifest.h"
#include "runtime/platform/checkpoint/checkpoint_store.h"
#include "runtime/platform/checkpoint/durable_writer.h"
#include "runtime/platform/checkpoint/local_filesystem_checkpoint_store.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/platform/provenance/local_merkle_dag_store.h"
#include "runtime/platform/provenance/merkle_dag_store.h"
#include "runtime/platform/provenance/provenance_query.h"
#include "runtime/util/status_macros.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

constexpr absl::string_view kTenant = "tenant-a";
constexpr absl::string_view kSession = "session-1";
constexpr absl::string_view kSchemaId = "incident_response_v1";
constexpr absl::string_view kSchemaJson =
    R"json({"Facts":[],"Reasoning":[],"Compliance":[]})json";

std::filesystem::path TestRoot(absl::string_view name) {
  std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / std::string(name);
  std::filesystem::remove_all(path);
  return path;
}

Hash256 H(absl::string_view bytes) {
  return HashBytes(HashAlgorithm::kBlake3, bytes);
}

std::vector<std::string> MakeEvents(int count) {
  std::vector<std::string> events;
  events.reserve(count);
  for (int i = 0; i < count; ++i) {
    events.push_back(absl::StrCat("[", i + 1,
                                  "] {\"type\":\"tool\",\"payload\":\"event-",
                                  i, " observed T10", i % 10, "\"}"));
  }
  return events;
}

std::string JoinEvents(const std::vector<std::string>& events, int begin,
                       int end) {
  std::string out;
  for (int i = begin; i < end; ++i) {
    absl::StrAppend(&out, events[i], "\n");
  }
  return out;
}

std::string DeterministicProjectionForRange(
    const std::vector<std::string>& events, int begin, int end) {
  auto parts = CreateProjectionPromptParts(JoinEvents(events, begin, end),
                                           kSchemaId, kSchemaJson, 1338,
                                           1 << 20);
  return parts.value().Compose();
}

CanonicalManifestInput BaselineManifestInput() {
  CanonicalManifestInput in;
  in.tenant_id = std::string(kTenant);
  in.session_id = std::string(kSession);
  in.branch_id = "main";
  in.compaction_interval = 8;
  in.architecture_tag = "x86_64-cpu";
  in.producer_id = "phase2-substrate-property-test";
  in.runtime_version = "test-runtime";
  in.model_artifact_hash = H("model-bytes");
  in.model_id = "pinned-local-test-model";
  in.model_class = 2;
  in.num_layers = 28;
  in.num_kv_heads = 8;
  in.head_dim = 128;
  in.kv_dtype = 1;
  in.created_unix_micros = 1777390000000000;
  return in;
}

CanonicalManifestInput ManifestForRange(
    const std::vector<std::string>& events, int begin, int end,
    const std::vector<Hash256>& parent_hashes = {}, uint32_t level = 0) {
  CanonicalManifestInput in = BaselineManifestInput();
  const std::string projection = DeterministicProjectionForRange(events, begin,
                                                                 end);
  in.level = level;
  in.parent_hashes = parent_hashes;
  in.base_event_index = static_cast<uint64_t>(end);
  in.body_hash = H(projection);
  in.body_size_bytes = static_cast<uint32_t>(projection.size());
  return in;
}

struct StoredCheckpoint {
  CanonicalManifestInput input;
  Hash256 manifest_hash;
  Hash256 body_hash;
  std::string payload;
};

absl::StatusOr<StoredCheckpoint> StoreCheckpoint(
    LocalFilesystemCheckpointStore* store, LocalMerkleDagStore* dag,
    const CanonicalManifestInput& input, absl::string_view payload) {
  StoredCheckpoint stored;
  stored.input = input;
  stored.payload = std::string(payload);
  ASSIGN_OR_RETURN(stored.body_hash,
                   store->PutPayload(kTenant, kSession, payload,
                                     HashAlgorithm::kBlake3));
  if (!(stored.body_hash == input.body_hash)) {
    return absl::InternalError("test helper body_hash mismatch");
  }
  ASSIGN_OR_RETURN(stored.manifest_hash,
                   ComputeManifestHash(HashAlgorithm::kBlake3, input));
  ASSIGN_OR_RETURN(std::string abi_bytes, EncodeCanonicalManifest(input));
  RETURN_IF_ERROR(store->PutManifest(kTenant, kSession, stored.manifest_hash,
                                     abi_bytes, stored.body_hash));
  RETURN_IF_ERROR(dag->Put(kTenant, kSession,
                           MerkleDagNode{
                               .hash = stored.manifest_hash,
                               .parent_hashes = input.parent_hashes,
                               .created_unix_micros =
                                   input.created_unix_micros,
                               .annotations = absl::StrCat(
                                   "base_event_index=",
                                   input.base_event_index),
                           }));
  return stored;
}

std::vector<StoredCheckpoint> StoreTenCheckpointRanges(
    const std::vector<std::string>& events,
    LocalFilesystemCheckpointStore* store, LocalMerkleDagStore* dag) {
  std::vector<StoredCheckpoint> checkpoints;
  checkpoints.reserve(10);
  std::vector<Hash256> parents;
  for (int end = 10; end <= 100; end += 10) {
    const int begin = end - 10;
    CanonicalManifestInput input =
        ManifestForRange(events, begin, end, parents);
    auto stored = StoreCheckpoint(
        store, dag, input, DeterministicProjectionForRange(events, begin, end));
    parents = {stored.value().manifest_hash};
    checkpoints.push_back(std::move(*stored));
  }
  return checkpoints;
}

TEST(Phase2SubstratePropertyTest, P1ReplayDeterminismPromptBytesStable) {
  const std::string event_log = JoinEvents(MakeEvents(6), 0, 6);
  ASSERT_OK_AND_ASSIGN(
      ProjectionPromptParts first,
      CreateProjectionPromptParts(event_log, kSchemaId, kSchemaJson, 1338,
                                  1 << 20));
  const Hash256 prefix_hash = H(first.cacheable_prefix);
  const Hash256 suffix_hash = H(first.event_log_suffix);
  const std::string composed = first.Compose();

  for (int replay = 0; replay < 10; ++replay) {
    ASSERT_OK_AND_ASSIGN(
        ProjectionPromptParts parts,
        CreateProjectionPromptParts(event_log, kSchemaId, kSchemaJson, 1338,
                                    1 << 20));
    EXPECT_EQ(H(parts.cacheable_prefix), prefix_hash);
    EXPECT_EQ(H(parts.event_log_suffix), suffix_hash);
    EXPECT_EQ(parts.Compose(), composed);
  }
}

TEST(Phase2SubstratePropertyTest, P2ManifestAuthorityCoversHeaderFields) {
  const CanonicalManifestInput base = BaselineManifestInput();
  ASSERT_OK_AND_ASSIGN(Hash256 base_hash,
                       ComputeManifestHash(HashAlgorithm::kBlake3, base));

  std::vector<CanonicalManifestInput> mutations;
  mutations.push_back(base);
  mutations.back().tenant_id = "tenant-b";
  mutations.push_back(base);
  mutations.back().branch_id = "handoff";
  mutations.push_back(base);
  mutations.back().level = 2;
  mutations.push_back(base);
  mutations.back().parent_hashes = {H("parent")};
  mutations.push_back(base);
  mutations.back().architecture_tag = "arm64-npu";
  mutations.push_back(base);
  mutations.back().model_artifact_hash = H("other-model");
  mutations.push_back(base);
  mutations.back().model_id = "other-model-id";
  mutations.push_back(base);
  mutations.back().kv_dtype = 2;
  mutations.push_back(base);
  mutations.back().base_event_index = 42;
  mutations.push_back(base);
  mutations.back().body_hash = H("other-body");
  mutations.push_back(base);
  mutations.back().body_size_bytes = 7;
  mutations.push_back(base);
  mutations.back().created_unix_micros += 1;

  for (const CanonicalManifestInput& mutation : mutations) {
    ASSERT_OK_AND_ASSIGN(Hash256 h,
                         ComputeManifestHash(HashAlgorithm::kBlake3,
                                             mutation));
    EXPECT_NE(h, base_hash);
  }
}

TEST(Phase2SubstratePropertyTest, P3MerkleIntegrityDetectsMutatedAncestor) {
  const std::filesystem::path root = TestRoot("phase2_p3_merkle_integrity");
  LocalMerkleDagStore dag(root);
  std::vector<Hash256> hashes;
  hashes.reserve(5);
  for (int i = 0; i < 5; ++i) {
    MerkleDagNode node;
    node.hash = H(absl::StrCat("node-", i));
    if (!hashes.empty()) node.parent_hashes = {hashes.back()};
    node.created_unix_micros = 100 + i;
    ASSERT_OK(dag.Put(kTenant, kSession, node));
    hashes.push_back(node.hash);
  }

  ASSERT_OK_AND_ASSIGN(ProvenanceChain chain,
                       GetCheckpointProvenance(dag, kTenant, kSession,
                                               hashes.back()));
  ASSERT_EQ(chain.nodes.size(), 5);
  EXPECT_EQ(chain.nodes.front().hash, hashes.back());
  EXPECT_EQ(chain.nodes.back().hash, hashes.front());

  std::filesystem::path parent_path = dag.PathFor(kTenant, kSession, hashes[2]);
  std::string bytes;
  ASSERT_OK(ReadEntireFileIfExists(parent_path, &bytes));
  ASSERT_GT(bytes.size(), 48);
  bytes[48] = static_cast<char>(bytes[48] ^ 0x01);
  ASSERT_OK(DurablyWriteFile(parent_path, bytes));

  EXPECT_FALSE(GetCheckpointProvenance(dag, kTenant, kSession, hashes.back())
                   .ok());
}

TEST(Phase2SubstratePropertyTest, P4CheckpointRangesHaveNoGapsOrOverlaps) {
  LocalFilesystemCheckpointStore store(TestRoot("phase2_p4_ckpt"));
  LocalMerkleDagStore dag(TestRoot("phase2_p4_dag"));
  const std::vector<std::string> events = MakeEvents(100);
  const std::vector<StoredCheckpoint> checkpoints =
      StoreTenCheckpointRanges(events, &store, &dag);

  std::vector<int> coverage(events.size(), 0);
  uint64_t begin = 0;
  for (const StoredCheckpoint& checkpoint : checkpoints) {
    ASSERT_GT(checkpoint.input.base_event_index, begin);
    ASSERT_LE(checkpoint.input.base_event_index, events.size());
    for (uint64_t i = begin; i < checkpoint.input.base_event_index; ++i) {
      coverage[static_cast<size_t>(i)]++;
    }
    begin = checkpoint.input.base_event_index;
  }
  EXPECT_EQ(begin, events.size());
  for (int count : coverage) {
    EXPECT_EQ(count, 1);
  }
}

TEST(Phase2SubstratePropertyTest, P5ReplayFromRawRecomputesStoredManifest) {
  LocalFilesystemCheckpointStore store(TestRoot("phase2_p5_ckpt"));
  LocalMerkleDagStore dag(TestRoot("phase2_p5_dag"));
  const std::vector<std::string> events = MakeEvents(100);
  const int begin = 20;
  const int end = 30;
  const std::string original_projection =
      DeterministicProjectionForRange(events, begin, end);
  const CanonicalManifestInput original_input =
      ManifestForRange(events, begin, end);
  ASSERT_OK_AND_ASSIGN(
      const StoredCheckpoint stored,
      StoreCheckpoint(&store, &dag, original_input, original_projection));

  ASSERT_OK_AND_ASSIGN(CheckpointStore::ManifestRecord manifest,
                       store.GetManifest(kTenant, kSession,
                                         stored.manifest_hash));
  ASSERT_OK_AND_ASSIGN(std::string stored_payload,
                       store.GetPayload(kTenant, kSession,
                                        manifest.referenced_body_hash));
  EXPECT_EQ(stored_payload, original_projection);

  const std::string replay_projection =
      DeterministicProjectionForRange(events, begin, end);
  CanonicalManifestInput replay_input = original_input;
  replay_input.body_hash = H(replay_projection);
  replay_input.body_size_bytes =
      static_cast<uint32_t>(replay_projection.size());
  ASSERT_OK_AND_ASSIGN(Hash256 replay_manifest_hash,
                       ComputeManifestHash(HashAlgorithm::kBlake3,
                                           replay_input));
  EXPECT_EQ(replay_manifest_hash, stored.manifest_hash);
  EXPECT_EQ(H(replay_projection), manifest.referenced_body_hash);
  ASSERT_OK_AND_ASSIGN(std::string replay_abi,
                       EncodeCanonicalManifest(replay_input));
  EXPECT_EQ(replay_abi, manifest.abi_bytes);
}

TEST(Phase2SubstratePropertyTest, P6CrossContextRollupSurvivesStoreReplication) {
  const std::filesystem::path root_a = TestRoot("phase2_p6_a");
  const std::filesystem::path root_b = TestRoot("phase2_p6_b");
  LocalFilesystemCheckpointStore store_a(root_a);
  LocalMerkleDagStore dag_a(root_a);
  const std::vector<std::string> events = MakeEvents(100);
  const std::vector<StoredCheckpoint> leaves =
      StoreTenCheckpointRanges(events, &store_a, &dag_a);

  std::vector<Hash256> leaf_hashes;
  std::string rollup_payload;
  for (const StoredCheckpoint& leaf : leaves) {
    leaf_hashes.push_back(leaf.manifest_hash);
    absl::StrAppend(&rollup_payload, leaf.manifest_hash.ToHex(), "\n");
  }
  CanonicalManifestInput root_input = BaselineManifestInput();
  root_input.level = 1;
  root_input.parent_hashes = leaf_hashes;
  root_input.base_event_index = events.size();
  root_input.body_hash = H(rollup_payload);
  root_input.body_size_bytes = static_cast<uint32_t>(rollup_payload.size());
  ASSERT_OK_AND_ASSIGN(
      const StoredCheckpoint root,
      StoreCheckpoint(&store_a, &dag_a, root_input, rollup_payload));

  std::filesystem::copy(root_a, root_b,
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::overwrite_existing);
  LocalFilesystemCheckpointStore store_b(root_b);
  LocalMerkleDagStore dag_b(root_b);

  ASSERT_OK_AND_ASSIGN(ProvenanceChain chain_b,
                       GetCheckpointProvenance(dag_b, kTenant, kSession,
                                               root.manifest_hash));
  ASSERT_EQ(chain_b.nodes.size(), leaves.size() + 1);
  ASSERT_OK_AND_ASSIGN(CheckpointStore::ManifestRecord root_record_b,
                       store_b.GetManifest(kTenant, kSession,
                                           root.manifest_hash));
  ASSERT_OK_AND_ASSIGN(std::string root_payload_b,
                       store_b.GetPayload(kTenant, kSession,
                                          root_record_b.referenced_body_hash));
  EXPECT_EQ(root_payload_b, rollup_payload);

  std::vector<Hash256> replicated_leaf_hashes;
  for (const MerkleDagNode& node : chain_b.nodes) {
    if (node.hash == root.manifest_hash) continue;
    replicated_leaf_hashes.push_back(node.hash);
  }
  std::sort(replicated_leaf_hashes.begin(), replicated_leaf_hashes.end());
  std::sort(leaf_hashes.begin(), leaf_hashes.end());
  EXPECT_EQ(replicated_leaf_hashes, leaf_hashes);

  CanonicalManifestInput root_rederived = root_input;
  ASSERT_OK_AND_ASSIGN(Hash256 root_hash_b,
                       ComputeManifestHash(HashAlgorithm::kBlake3,
                                           root_rederived));
  EXPECT_EQ(root_hash_b, root.manifest_hash);
}

}  // namespace
}  // namespace litert::lm
