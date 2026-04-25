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

#include <filesystem>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gtest/gtest.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/platform/provenance/local_merkle_dag_store.h"
#include "runtime/platform/provenance/merkle_dag_store.h"
#include "runtime/platform/provenance/provenance_query.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

std::filesystem::path TestRoot(absl::string_view name) {
  std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / std::string(name);
  std::filesystem::remove_all(path);
  return path;
}

// Helper: compute a node's "hash" deterministically from a label so test
// nodes pin to known addresses.
Hash256 LabelHash(absl::string_view label) {
  return HashBytes(HashAlgorithm::kBlake3, label);
}

TEST(LocalMerkleDagStoreTest, PutGetRoundTrip) {
  LocalMerkleDagStore store(TestRoot("dag_round_trip"));
  MerkleDagNode genesis;
  genesis.hash = LabelHash("genesis");
  genesis.created_unix_micros = 100;
  genesis.annotations = "trigger=manual";
  ASSERT_OK(store.Put("tenant-a", "session-1", genesis));

  ASSERT_OK_AND_ASSIGN(MerkleDagNode got,
                       store.Get("tenant-a", "session-1", genesis.hash));
  EXPECT_EQ(got.hash, genesis.hash);
  EXPECT_EQ(got.parent_hashes.size(), 0);
  EXPECT_EQ(got.created_unix_micros, 100);
  EXPECT_EQ(got.annotations, "trigger=manual");
}

TEST(LocalMerkleDagStoreTest, RoundTripWithMultipleParents) {
  LocalMerkleDagStore store(TestRoot("dag_multi_parent"));
  MerkleDagNode merge;
  merge.hash = LabelHash("merge");
  merge.parent_hashes = {LabelHash("a"), LabelHash("b"), LabelHash("c")};
  merge.created_unix_micros = 555;
  ASSERT_OK(store.Put("tenant-a", "session-1", merge));
  ASSERT_OK_AND_ASSIGN(MerkleDagNode got,
                       store.Get("tenant-a", "session-1", merge.hash));
  ASSERT_EQ(got.parent_hashes.size(), 3);
  EXPECT_EQ(got.parent_hashes[0], LabelHash("a"));
  EXPECT_EQ(got.parent_hashes[2], LabelHash("c"));
}

TEST(LocalMerkleDagStoreTest, MissingNodeIsNotFound) {
  LocalMerkleDagStore store(TestRoot("dag_404"));
  Hash256 fake;
  EXPECT_EQ(store.Get("tenant-a", "session-1", fake).status().code(),
            absl::StatusCode::kNotFound);
}

TEST(ProvenanceQueryTest, LinearChainReturnsLeafFirst) {
  LocalMerkleDagStore store(TestRoot("prov_linear"));
  // genesis -> n1 -> n2 -> leaf
  MerkleDagNode genesis{
      .hash = LabelHash("genesis"), .created_unix_micros = 1};
  MerkleDagNode n1{
      .hash = LabelHash("n1"),
      .parent_hashes = {LabelHash("genesis")},
      .created_unix_micros = 2};
  MerkleDagNode n2{
      .hash = LabelHash("n2"),
      .parent_hashes = {LabelHash("n1")},
      .created_unix_micros = 3};
  MerkleDagNode leaf{
      .hash = LabelHash("leaf"),
      .parent_hashes = {LabelHash("n2")},
      .created_unix_micros = 4};
  ASSERT_OK(store.Put("t", "s", genesis));
  ASSERT_OK(store.Put("t", "s", n1));
  ASSERT_OK(store.Put("t", "s", n2));
  ASSERT_OK(store.Put("t", "s", leaf));

  ASSERT_OK_AND_ASSIGN(ProvenanceChain chain,
                       GetCheckpointProvenance(store, "t", "s", leaf.hash));
  ASSERT_EQ(chain.nodes.size(), 4);
  EXPECT_EQ(chain.nodes.front().hash, leaf.hash);
  EXPECT_EQ(chain.nodes.back().hash, genesis.hash);
}

TEST(ProvenanceQueryTest, MergePointVisitedOnce) {
  // A diamond DAG:
  //          genesis
  //          /     \
  //         L       R
  //          \     /
  //           merge
  LocalMerkleDagStore store(TestRoot("prov_diamond"));
  MerkleDagNode genesis{.hash = LabelHash("g")};
  MerkleDagNode L{
      .hash = LabelHash("L"), .parent_hashes = {LabelHash("g")}};
  MerkleDagNode R{
      .hash = LabelHash("R"), .parent_hashes = {LabelHash("g")}};
  MerkleDagNode merge{
      .hash = LabelHash("M"),
      .parent_hashes = {LabelHash("L"), LabelHash("R")}};
  ASSERT_OK(store.Put("t", "s", genesis));
  ASSERT_OK(store.Put("t", "s", L));
  ASSERT_OK(store.Put("t", "s", R));
  ASSERT_OK(store.Put("t", "s", merge));

  ASSERT_OK_AND_ASSIGN(ProvenanceChain chain,
                       GetCheckpointProvenance(store, "t", "s", merge.hash));
  // The diamond has 4 unique nodes. The genesis must appear once even
  // though it is reachable through both L and R.
  EXPECT_EQ(chain.nodes.size(), 4);
  EXPECT_EQ(chain.nodes.front().hash, merge.hash);
  EXPECT_EQ(chain.nodes.back().hash, genesis.hash);
}

TEST(ProvenanceQueryTest, DeterministicAcrossRepeatedRuns) {
  // Two runs over the same DAG must produce byte-identical chains.
  LocalMerkleDagStore store(TestRoot("prov_determ"));
  MerkleDagNode g{.hash = LabelHash("g")};
  MerkleDagNode a{.hash = LabelHash("a"), .parent_hashes = {LabelHash("g")}};
  MerkleDagNode b{.hash = LabelHash("b"), .parent_hashes = {LabelHash("g")}};
  MerkleDagNode m{
      .hash = LabelHash("m"),
      .parent_hashes = {LabelHash("a"), LabelHash("b")}};
  ASSERT_OK(store.Put("t", "s", g));
  ASSERT_OK(store.Put("t", "s", a));
  ASSERT_OK(store.Put("t", "s", b));
  ASSERT_OK(store.Put("t", "s", m));

  ASSERT_OK_AND_ASSIGN(ProvenanceChain c1,
                       GetCheckpointProvenance(store, "t", "s", m.hash));
  ASSERT_OK_AND_ASSIGN(ProvenanceChain c2,
                       GetCheckpointProvenance(store, "t", "s", m.hash));
  ASSERT_EQ(c1.nodes.size(), c2.nodes.size());
  for (size_t i = 0; i < c1.nodes.size(); ++i) {
    EXPECT_EQ(c1.nodes[i].hash, c2.nodes[i].hash);
  }
}

TEST(ProvenanceQueryTest, MissingAncestorPropagatesAsNotFound) {
  LocalMerkleDagStore store(TestRoot("prov_missing"));
  MerkleDagNode child{
      .hash = LabelHash("child"), .parent_hashes = {LabelHash("ghost")}};
  ASSERT_OK(store.Put("t", "s", child));
  // The "ghost" parent was never written.
  EXPECT_EQ(GetCheckpointProvenance(store, "t", "s", child.hash)
                .status()
                .code(),
            absl::StatusCode::kNotFound);
}

}  // namespace
}  // namespace litert::lm
