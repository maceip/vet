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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_PROVENANCE_LOCAL_MERKLE_DAG_STORE_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_PROVENANCE_LOCAL_MERKLE_DAG_STORE_H_

#include <filesystem>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/hash/hasher.h"
#include "runtime/platform/provenance/merkle_dag_store.h"

namespace litert::lm {

// Simple filesystem-backed DAG store. Layout:
//   root / tenant / session / dag / <hex_hash>.dagnode
//
// Each .dagnode is a small framed binary record containing the parent
// hash list, the created_unix_micros stamp, and an opaque annotations
// blob. The runtime does not depend on any JSON or protobuf library at
// this layer; the format is hand-rolled and matches the rest of the
// platform substrate's framing style (length-prefixed records with a
// magic header).
class LocalMerkleDagStore : public MerkleDagStore {
 public:
  explicit LocalMerkleDagStore(std::filesystem::path root_path);

  absl::Status Put(absl::string_view tenant_id,
                   absl::string_view session_id,
                   const MerkleDagNode& node) override;

  absl::StatusOr<MerkleDagNode> Get(absl::string_view tenant_id,
                                    absl::string_view session_id,
                                    const Hash256& hash) const override;

  absl::StatusOr<bool> Exists(absl::string_view tenant_id,
                              absl::string_view session_id,
                              const Hash256& hash) const override;

  std::filesystem::path PathFor(absl::string_view tenant_id,
                                absl::string_view session_id,
                                const Hash256& hash) const;

 private:
  std::filesystem::path root_path_;
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_PROVENANCE_LOCAL_MERKLE_DAG_STORE_H_
