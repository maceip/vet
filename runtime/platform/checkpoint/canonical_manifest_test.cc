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

#include "runtime/platform/checkpoint/canonical_manifest.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

CanonicalManifestInput Baseline() {
  CanonicalManifestInput in;
  in.tenant_id = "tenant-a";
  in.session_id = "session-1";
  in.branch_id = "main";
  in.level = 0;
  in.compaction_interval = 8;
  in.architecture_tag = "arm64-hexagon-int8";
  in.producer_id = "node-7";
  in.runtime_version = "litertlm-1.2.3";
  in.model_artifact_hash =
      HashBytes(HashAlgorithm::kBlake3, "model-bundle-bytes");
  in.model_id = "gemma-3-test";
  in.model_class = 2;  // GQA
  in.num_layers = 24;
  in.num_kv_heads = 8;
  in.head_dim = 128;
  in.kv_dtype = 2;  // int8 per token
  in.base_event_index = 100;
  in.body_hash = HashBytes(HashAlgorithm::kBlake3, "payload-bytes");
  in.body_size_bytes = 12345;
  in.created_unix_micros = 1777089600000000;
  return in;
}

TEST(CanonicalManifestTest, EncodingIsByteDeterministic) {
  ASSERT_OK_AND_ASSIGN(std::string a, EncodeCanonicalManifest(Baseline()));
  ASSERT_OK_AND_ASSIGN(std::string b, EncodeCanonicalManifest(Baseline()));
  EXPECT_EQ(a, b);
}

TEST(CanonicalManifestTest, ManifestHashCoversIdentityFields) {
  ASSERT_OK_AND_ASSIGN(Hash256 base,
                       ComputeManifestHash(HashAlgorithm::kBlake3, Baseline()));
  for (auto perturb : std::vector<std::function<void(CanonicalManifestInput&)>>{
           [](auto& x) { x.tenant_id = "tenant-b"; },
           [](auto& x) { x.session_id = "session-2"; },
           [](auto& x) { x.branch_id = "investigation"; },
       }) {
    CanonicalManifestInput in = Baseline();
    perturb(in);
    ASSERT_OK_AND_ASSIGN(Hash256 h,
                         ComputeManifestHash(HashAlgorithm::kBlake3, in));
    EXPECT_NE(h, base);
  }
}

TEST(CanonicalManifestTest, ManifestHashCoversModelAndBackendFields) {
  ASSERT_OK_AND_ASSIGN(Hash256 base,
                       ComputeManifestHash(HashAlgorithm::kBlake3, Baseline()));
  for (auto perturb : std::vector<std::function<void(CanonicalManifestInput&)>>{
           [](auto& x) { x.architecture_tag = "x86_64-cuda-fp16"; },
           [](auto& x) { x.producer_id = "node-99"; },
           [](auto& x) { x.runtime_version = "different"; },
           [](auto& x) {
             x.model_artifact_hash =
                 HashBytes(HashAlgorithm::kBlake3, "different-model");
           },
           [](auto& x) { x.model_id = "phi-4"; },
           [](auto& x) { x.model_class = 3; },
           [](auto& x) { x.num_kv_heads = 16; },
           [](auto& x) { x.head_dim = 64; },
           [](auto& x) { x.kv_dtype = 1; },
       }) {
    CanonicalManifestInput in = Baseline();
    perturb(in);
    ASSERT_OK_AND_ASSIGN(Hash256 h,
                         ComputeManifestHash(HashAlgorithm::kBlake3, in));
    EXPECT_NE(h, base);
  }
}

TEST(CanonicalManifestTest, ManifestHashCoversParentHashesAndBody) {
  ASSERT_OK_AND_ASSIGN(Hash256 base,
                       ComputeManifestHash(HashAlgorithm::kBlake3, Baseline()));

  CanonicalManifestInput add_parent = Baseline();
  add_parent.parent_hashes.push_back(
      HashBytes(HashAlgorithm::kBlake3, "parent-1"));
  ASSERT_OK_AND_ASSIGN(Hash256 h1,
                       ComputeManifestHash(HashAlgorithm::kBlake3, add_parent));
  EXPECT_NE(h1, base);

  CanonicalManifestInput swap_parents = Baseline();
  swap_parents.parent_hashes = {
      HashBytes(HashAlgorithm::kBlake3, "p1"),
      HashBytes(HashAlgorithm::kBlake3, "p2"),
  };
  CanonicalManifestInput swap_parents_reordered = swap_parents;
  std::swap(swap_parents_reordered.parent_hashes[0],
            swap_parents_reordered.parent_hashes[1]);
  ASSERT_OK_AND_ASSIGN(
      Hash256 ordered,
      ComputeManifestHash(HashAlgorithm::kBlake3, swap_parents));
  ASSERT_OK_AND_ASSIGN(
      Hash256 reordered,
      ComputeManifestHash(HashAlgorithm::kBlake3, swap_parents_reordered));
  // Order of parent_hashes is preserved in the encoding so producers can
  // reflect their walk order in the digest.
  EXPECT_NE(ordered, reordered);

  CanonicalManifestInput diff_body = Baseline();
  diff_body.body_hash = HashBytes(HashAlgorithm::kBlake3, "different-body");
  ASSERT_OK_AND_ASSIGN(Hash256 h2,
                       ComputeManifestHash(HashAlgorithm::kBlake3, diff_body));
  EXPECT_NE(h2, base);
}

TEST(CanonicalManifestTest, AlgorithmAffectsHashButNotEncoding) {
  ASSERT_OK_AND_ASSIGN(std::string bytes_a,
                       EncodeCanonicalManifest(Baseline()));
  ASSERT_OK_AND_ASSIGN(std::string bytes_b,
                       EncodeCanonicalManifest(Baseline()));
  EXPECT_EQ(bytes_a, bytes_b);
  ASSERT_OK_AND_ASSIGN(Hash256 b3,
                       ComputeManifestHash(HashAlgorithm::kBlake3, Baseline()));
  ASSERT_OK_AND_ASSIGN(Hash256 sha,
                       ComputeManifestHash(HashAlgorithm::kSha256, Baseline()));
  EXPECT_NE(b3, sha);
}

}  // namespace
}  // namespace litert::lm
