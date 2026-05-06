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

#include "runtime/platform/provenance/provenance_query.h"

#include <algorithm>
#include <queue>
#include <vector>

#include "absl/container/flat_hash_set.h"  // from @com_google_absl
#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "runtime/platform/hash/hasher.h"
#include "runtime/platform/provenance/merkle_dag_store.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {

absl::StatusOr<ProvenanceChain> GetCheckpointProvenance(
    const MerkleDagStore& store, absl::string_view tenant_id,
    absl::string_view session_id, const Hash256& leaf_hash) {
  ProvenanceChain chain;
  absl::flat_hash_set<Hash256> visited;
  std::vector<Hash256> current_level{leaf_hash};

  while (!current_level.empty()) {
    if (chain.nodes.size() + current_level.size() >
        ProvenanceChain::kMaxProvenanceNodes) {
      return absl::ResourceExhaustedError(absl::StrCat(
          "provenance traversal exceeded kMaxProvenanceNodes (",
          ProvenanceChain::kMaxProvenanceNodes, ")."));
    }
    // Deterministic order within a level.
    std::sort(current_level.begin(), current_level.end());
    std::vector<Hash256> next_level;
    next_level.reserve(current_level.size());
    for (const Hash256& h : current_level) {
      if (!visited.insert(h).second) continue;  // already drained
      ASSIGN_OR_RETURN(MerkleDagNode node,
                       store.Get(tenant_id, session_id, h));
      for (const Hash256& parent : node.parent_hashes) {
        if (!visited.contains(parent)) {
          next_level.push_back(parent);
        }
      }
      chain.nodes.push_back(std::move(node));
    }
    current_level = std::move(next_level);
  }
  return chain;
}

}  // namespace litert::lm
