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

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gtest/gtest.h"
#include "runtime/dpm/projection_prompt.h"
#include "runtime/platform/checkpoint/canonical_manifest.h"
#include "runtime/platform/checkpoint/checkpoint_store.h"
#include "runtime/platform/checkpoint/durable_writer.h"
#include "runtime/platform/checkpoint/local_filesystem_checkpoint_store.h"
#include "runtime/platform/checkpoint/testing/session_case_loader.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/platform/provenance/local_merkle_dag_store.h"
#include "runtime/platform/provenance/merkle_dag_store.h"
#include "runtime/platform/provenance/provenance_query.h"
#include "runtime/util/status_macros.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

using ::litert::lm::bench::LoadSessionCasesFromFile;
using ::litert::lm::bench::RenderEventLog;
using ::litert::lm::bench::SessionCase;

constexpr absl::string_view kFixturePath =
    "runtime/platform/checkpoint/testing/golden/curated_session_cases.json";
constexpr absl::string_view kTenant = "tenant-a";
constexpr absl::string_view kSchemaId = "session_replay_v1";
constexpr absl::string_view kSchemaJson =
    R"json({"intent":"string","open_decisions":[],"corrections":[]})json";
constexpr uint32_t kMemoryBudgetChars = 5352;

std::filesystem::path TestRoot(absl::string_view name) {
  std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / std::string(name);
  std::filesystem::remove_all(path);
  return path;
}

Hash256 H(absl::string_view bytes) {
  return HashBytes(HashAlgorithm::kBlake3, bytes);
}

std::string SafeSessionId(const SessionCase& c) {
  std::string out = c.case_id;
  for (char& ch : out) {
    if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') &&
        !(ch >= '0' && ch <= '9') && ch != '-' && ch != '_') {
      ch = '_';
    }
  }
  return out;
}

absl::StatusOr<ProjectionPromptParts> PromptForEventLog(
    absl::string_view event_log) {
  return CreateProjectionPromptParts(event_log, kSchemaId, kSchemaJson,
                                     kMemoryBudgetChars,
                                     /*max_event_log_chars=*/1 << 20);
}

std::string ProjectionForEventLog(absl::string_view event_log) {
  return PromptForEventLog(event_log).value().Compose();
}

std::string RenderRange(const SessionCase& c, int begin, int end) {
  SessionCase slice = c;
  slice.events.clear();
  for (int i = begin; i < end; ++i) {
    slice.events.push_back(c.events[static_cast<size_t>(i)]);
  }
  return RenderEventLog(slice);
}

CanonicalManifestInput ManifestBaseForCase(const SessionCase& c) {
  CanonicalManifestInput in;
  in.tenant_id = std::string(kTenant);
  in.session_id = SafeSessionId(c);
  in.branch_id = "main";
  in.compaction_interval = 8;
  in.architecture_tag = "x86_64-cpu";
  in.producer_id = "phase2-session-case-property-test";
  in.runtime_version = "test-runtime";
  in.model_artifact_hash = H("pinned-local-test-model-bytes");
  in.model_id = "pinned-local-test-model";
  in.schema_id = std::string(kSchemaId);
  in.schema_hash = H(kSchemaJson);
  in.model_class = 2;
  in.num_layers = 28;
  in.num_kv_heads = 8;
  in.head_dim = 128;
  in.kv_dtype = 1;
  in.created_unix_micros = 1777390000000000;
  return in;
}

CanonicalManifestInput ManifestForProjection(
    const SessionCase& c, uint64_t event_range_start,
    uint64_t event_range_end,
    const std::vector<Hash256>& parents, uint32_t level,
    absl::string_view projection) {
  CanonicalManifestInput in = ManifestBaseForCase(c);
  in.parent_hashes = parents;
  in.level = level;
  in.event_range_start = event_range_start;
  in.event_range_end = event_range_end;
  in.base_event_index = event_range_end;
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
    const SessionCase& c, const CanonicalManifestInput& input,
    absl::string_view payload) {
  StoredCheckpoint stored;
  stored.input = input;
  stored.payload = std::string(payload);
  ASSIGN_OR_RETURN(stored.body_hash,
                   store->PutPayload(kTenant, input.session_id, payload,
                                     HashAlgorithm::kBlake3));
  if (!(stored.body_hash == input.body_hash)) {
    return absl::InternalError("test helper body_hash mismatch");
  }
  ASSIGN_OR_RETURN(stored.manifest_hash,
                   ComputeManifestHash(HashAlgorithm::kBlake3, input));
  ASSIGN_OR_RETURN(std::string abi_bytes, EncodeCanonicalManifest(input));
  RETURN_IF_ERROR(store->PutManifest(kTenant, input.session_id,
                                     stored.manifest_hash, abi_bytes,
                                     stored.body_hash));
  RETURN_IF_ERROR(dag->Put(kTenant, input.session_id,
                           MerkleDagNode{
                               .hash = stored.manifest_hash,
                               .parent_hashes = input.parent_hashes,
                               .created_unix_micros =
                                   input.created_unix_micros,
                               .annotations = absl::StrCat(
                                   "case_id=", c.case_id,
                                   ";event_range=[",
                                   input.event_range_start, ",",
                                   input.event_range_end, ")"),
                           }));
  return stored;
}

std::vector<SessionCase> LoadCuratedCases() {
  auto loaded = LoadSessionCasesFromFile(kFixturePath);
  EXPECT_TRUE(loaded.ok()) << loaded.status();
  if (!loaded.ok()) return {};
  EXPECT_EQ(loaded->size(), 5u);
  return *std::move(loaded);
}

std::vector<std::pair<int, int>> ContiguousRanges(int event_count) {
  std::vector<std::pair<int, int>> ranges;
  if (event_count == 0) return ranges;
  const int split = std::max(1, event_count / 3);
  for (int begin = 0; begin < event_count; begin += split) {
    ranges.push_back({begin, std::min(event_count, begin + split)});
  }
  return ranges;
}

TEST(Phase2SessionCasePropertyTest, P1ReplayDeterminismOverCuratedSessions) {
  for (const SessionCase& c : LoadCuratedCases()) {
    const std::string event_log = RenderEventLog(c);
    ASSERT_OK_AND_ASSIGN(ProjectionPromptParts first,
                         PromptForEventLog(event_log));
    const Hash256 prefix_hash = H(first.cacheable_prefix);
    const Hash256 suffix_hash = H(first.event_log_suffix);
    const std::string composed = first.Compose();

    for (int replay = 0; replay < 10; ++replay) {
      ASSERT_OK_AND_ASSIGN(ProjectionPromptParts parts,
                           PromptForEventLog(event_log));
      EXPECT_EQ(H(parts.cacheable_prefix), prefix_hash) << c.case_id;
      EXPECT_EQ(H(parts.event_log_suffix), suffix_hash) << c.case_id;
      EXPECT_EQ(parts.Compose(), composed) << c.case_id;
    }
  }
}

TEST(Phase2SessionCasePropertyTest, P2ManifestAuthorityBindsSessionFields) {
  for (const SessionCase& c : LoadCuratedCases()) {
    const std::string projection = ProjectionForEventLog(RenderEventLog(c));
    CanonicalManifestInput base = ManifestForProjection(
        c, 0, c.events.size(), /*parents=*/{}, /*level=*/0, projection);
    ASSERT_OK_AND_ASSIGN(Hash256 base_hash,
                         ComputeManifestHash(HashAlgorithm::kBlake3, base));

    CanonicalManifestInput mutated = base;
    mutated.session_id = absl::StrCat(mutated.session_id, "-other");
    ASSERT_OK_AND_ASSIGN(Hash256 session_hash,
                         ComputeManifestHash(HashAlgorithm::kBlake3,
                                             mutated));
    EXPECT_NE(session_hash, base_hash) << c.case_id;

    mutated = base;
    mutated.model_artifact_hash = H("other-model");
    ASSERT_OK_AND_ASSIGN(Hash256 model_hash,
                         ComputeManifestHash(HashAlgorithm::kBlake3,
                                             mutated));
    EXPECT_NE(model_hash, base_hash) << c.case_id;

    mutated = base;
    mutated.body_hash = H(absl::StrCat(projection, "tampered"));
    ASSERT_OK_AND_ASSIGN(Hash256 body_hash,
                         ComputeManifestHash(HashAlgorithm::kBlake3,
                                             mutated));
    EXPECT_NE(body_hash, base_hash) << c.case_id;
  }
}

TEST(Phase2SessionCasePropertyTest, P3MerkleIntegrityOverSessionAncestors) {
  for (const SessionCase& c : LoadCuratedCases()) {
    const std::filesystem::path root =
        TestRoot(absl::StrCat("phase2_session_p3_", SafeSessionId(c)));
    LocalFilesystemCheckpointStore store(root);
    LocalMerkleDagStore dag(root);
    const std::vector<std::pair<int, int>> ranges =
        ContiguousRanges(static_cast<int>(c.events.size()));

    std::vector<Hash256> parents;
    Hash256 first_hash;
    Hash256 last_hash;
    for (const auto& [begin, end] : ranges) {
      const std::string projection = ProjectionForEventLog(
          RenderRange(c, begin, end));
      CanonicalManifestInput input = ManifestForProjection(
          c, begin, end, parents, /*level=*/0, projection);
      ASSERT_OK_AND_ASSIGN(StoredCheckpoint stored,
                           StoreCheckpoint(&store, &dag, c, input,
                                           projection));
      if (parents.empty()) first_hash = stored.manifest_hash;
      parents = {stored.manifest_hash};
      last_hash = stored.manifest_hash;
    }

    ASSERT_OK_AND_ASSIGN(ProvenanceChain chain,
                         GetCheckpointProvenance(dag, kTenant, SafeSessionId(c),
                                                 last_hash));
    ASSERT_EQ(chain.nodes.size(), ranges.size()) << c.case_id;

    std::filesystem::path first_path =
        dag.PathFor(kTenant, SafeSessionId(c), first_hash);
    ASSERT_OK(DurablyWriteFile(first_path, "corrupt dag node"));

    EXPECT_FALSE(GetCheckpointProvenance(dag, kTenant, SafeSessionId(c),
                                         last_hash)
                     .ok())
        << c.case_id;
  }
}

TEST(Phase2SessionCasePropertyTest, P4RangeCoverageNoGapsOrOverlaps) {
  for (const SessionCase& c : LoadCuratedCases()) {
    std::vector<int> coverage(c.events.size(), 0);
    for (const auto& [begin, end] :
         ContiguousRanges(static_cast<int>(c.events.size()))) {
      for (int i = begin; i < end; ++i) {
        coverage[static_cast<size_t>(i)]++;
      }
    }
    for (int count : coverage) {
      EXPECT_EQ(count, 1) << c.case_id;
    }
  }
}

TEST(Phase2SessionCasePropertyTest, P5ReplayFromRawSessionRecomputesManifest) {
  for (const SessionCase& c : LoadCuratedCases()) {
    const std::filesystem::path root =
        TestRoot(absl::StrCat("phase2_session_p5_", SafeSessionId(c)));
    LocalFilesystemCheckpointStore store(root);
    LocalMerkleDagStore dag(root);
    const std::string original_projection =
        ProjectionForEventLog(RenderEventLog(c));
    const CanonicalManifestInput original_input = ManifestForProjection(
        c, 0, c.events.size(), /*parents=*/{}, /*level=*/0,
        original_projection);
    ASSERT_OK_AND_ASSIGN(
        StoredCheckpoint stored,
        StoreCheckpoint(&store, &dag, c, original_input, original_projection));

    auto manifest_or =
        store.GetManifest(kTenant, original_input.session_id,
                          stored.manifest_hash);
    ASSERT_TRUE(manifest_or.ok())
        << c.case_id << " session=" << original_input.session_id << " status="
        << manifest_or.status();
    CheckpointStore::ManifestRecord manifest = *manifest_or;
    ASSERT_OK_AND_ASSIGN(std::string stored_payload,
                         store.GetPayload(kTenant, original_input.session_id,
                                          manifest.referenced_body_hash));
    EXPECT_EQ(stored_payload, original_projection) << c.case_id;

    const std::string replay_projection = ProjectionForEventLog(RenderEventLog(c));
    CanonicalManifestInput replay_input = original_input;
    replay_input.body_hash = H(replay_projection);
    replay_input.body_size_bytes =
        static_cast<uint32_t>(replay_projection.size());
    ASSERT_OK_AND_ASSIGN(Hash256 replay_hash,
                         ComputeManifestHash(HashAlgorithm::kBlake3,
                                             replay_input));
    EXPECT_EQ(replay_hash, stored.manifest_hash) << c.case_id;
    ASSERT_OK_AND_ASSIGN(std::string replay_abi,
                         EncodeCanonicalManifest(replay_input));
    EXPECT_EQ(replay_abi, manifest.abi_bytes) << c.case_id;
  }
}

TEST(Phase2SessionCasePropertyTest, P6CrossContextRollupReplicates) {
  const std::vector<SessionCase> cases = LoadCuratedCases();
  for (size_t case_index = 0; case_index < cases.size(); ++case_index) {
    const SessionCase& c = cases[case_index];
    const std::filesystem::path root_a =
        TestRoot(absl::StrCat("p6a_", case_index));
    const std::filesystem::path root_b =
        TestRoot(absl::StrCat("p6b_", case_index));
    LocalFilesystemCheckpointStore store_a(root_a);
    LocalMerkleDagStore dag_a(root_a);
    std::filesystem::create_directories(root_a);

    std::vector<Hash256> leaf_hashes;
    std::string rollup_payload;
    for (const auto& [begin, end] :
         ContiguousRanges(static_cast<int>(c.events.size()))) {
      const std::string projection = ProjectionForEventLog(
          RenderRange(c, begin, end));
      const CanonicalManifestInput input = ManifestForProjection(
          c, begin, end, /*parents=*/{}, /*level=*/0, projection);
      ASSERT_OK_AND_ASSIGN(StoredCheckpoint leaf,
                           StoreCheckpoint(&store_a, &dag_a, c, input,
                                           projection));
      leaf_hashes.push_back(leaf.manifest_hash);
      absl::StrAppend(&rollup_payload, leaf.manifest_hash.ToHex(), "\n");
    }

    const CanonicalManifestInput root_input = ManifestForProjection(
        c, 0, c.events.size(), leaf_hashes, /*level=*/1, rollup_payload);
    ASSERT_OK_AND_ASSIGN(
        StoredCheckpoint root,
        StoreCheckpoint(&store_a, &dag_a, c, root_input, rollup_payload));

    std::filesystem::copy(root_a, root_b,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    LocalFilesystemCheckpointStore store_b(root_b);
    LocalMerkleDagStore dag_b(root_b);

    ASSERT_OK_AND_ASSIGN(ProvenanceChain chain_b,
                         GetCheckpointProvenance(dag_b, kTenant,
                                                 SafeSessionId(c),
                                                 root.manifest_hash));
    ASSERT_EQ(chain_b.nodes.size(), leaf_hashes.size() + 1) << c.case_id;
    ASSERT_OK_AND_ASSIGN(CheckpointStore::ManifestRecord root_record_b,
                         store_b.GetManifest(kTenant, SafeSessionId(c),
                                             root.manifest_hash));
    ASSERT_OK_AND_ASSIGN(std::string root_payload_b,
                         store_b.GetPayload(kTenant, SafeSessionId(c),
                                            root_record_b.referenced_body_hash));
    EXPECT_EQ(root_payload_b, rollup_payload) << c.case_id;
  }
}

}  // namespace
}  // namespace litert::lm
