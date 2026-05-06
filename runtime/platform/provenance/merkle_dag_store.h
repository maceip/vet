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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_PROVENANCE_MERKLE_DAG_STORE_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_PROVENANCE_MERKLE_DAG_STORE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {

// A node in the checkpoint provenance DAG. Each node represents one
// checkpoint blob; node.hash is the content address of that blob. The
// parent_hashes list defines the DAG edges; usually 1 parent (linear
// chain), 0 for the genesis Level-0 of a session, and 2+ for merge
// points (e.g. a compaction that consumed multiple subtrees, or a
// multi-source audit reconciliation).
//
// annotations is free-form opaque bytes the caller can use to record
// trigger-type, agent notes, or anything else useful for audit. The DAG
// store itself does not interpret it.
struct MerkleDagNode {
  Hash256 hash;
  std::vector<Hash256> parent_hashes;
  int64_t created_unix_micros = 0;
  std::string annotations;
};

class MerkleDagStore {
 public:
  virtual ~MerkleDagStore() = default;

  virtual absl::Status Put(absl::string_view tenant_id,
                           absl::string_view session_id,
                           const MerkleDagNode& node) = 0;

  virtual absl::StatusOr<MerkleDagNode> Get(
      absl::string_view tenant_id, absl::string_view session_id,
      const Hash256& hash) const = 0;

  virtual absl::StatusOr<bool> Exists(absl::string_view tenant_id,
                                      absl::string_view session_id,
                                      const Hash256& hash) const = 0;
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_PROVENANCE_MERKLE_DAG_STORE_H_
