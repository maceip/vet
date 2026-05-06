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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_PROVENANCE_PROVENANCE_QUERY_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_PROVENANCE_PROVENANCE_QUERY_H_

#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/hash/hasher.h"
#include "runtime/platform/provenance/merkle_dag_store.h"

namespace litert::lm {

// Result of a provenance query: the leaf-rooted ancestor DAG flattened to
// a topologically sorted vector. nodes[0] is the leaf; the last element is
// the deepest reachable genesis. Branch / merge points appear once and
// their parent_hashes describe the DAG edges; consumers can rebuild the
// DAG from `nodes` alone.
//
// The traversal is bounded to kMaxProvenanceNodes to defend against
// pathological cycles or malformed stores; that limit is per-query and
// not a permanent ceiling.
struct ProvenanceChain {
  static constexpr int kMaxProvenanceNodes = 1 << 20;  // 1Mi nodes
  std::vector<MerkleDagNode> nodes;
};

// Walks the DAG rooted at leaf_hash via a deterministic BFS. A node is
// included only once even if reachable through multiple paths (merge
// points). Order: visit each level top-down; within a level, sort by
// hash bytes to make the result deterministic across replays.
absl::StatusOr<ProvenanceChain> GetCheckpointProvenance(
    const MerkleDagStore& store, absl::string_view tenant_id,
    absl::string_view session_id, const Hash256& leaf_hash);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_PROVENANCE_PROVENANCE_QUERY_H_
