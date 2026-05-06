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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_CHECKPOINTED_PROJECTION_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_CHECKPOINTED_PROJECTION_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "runtime/dpm/dpm_projector.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/platform/checkpoint/canonical_manifest.h"
#include "runtime/platform/checkpoint/checkpoint_store.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/platform/provenance/merkle_dag_store.h"

namespace litert::lm {

struct ProjectionCheckpointConfig {
  DPMProjector::ProjectionConfig projection;
  std::string branch_id = "main";
  std::vector<Hash256> parent_manifest_hashes;
  Hash256 model_artifact_hash;
  std::string architecture_tag = "unknown";
  std::string producer_id = "litert-dpm";
  std::string runtime_version = "unknown";
  uint32_t level = 0;
  uint32_t compaction_interval = 0;
  uint32_t model_class = 0;
  uint32_t num_layers = 0;
  uint32_t num_kv_heads = 0;
  uint32_t head_dim = 0;
  uint32_t kv_dtype = 1;
  int64_t created_unix_micros = 0;
};

struct ProjectionCheckpoint {
  Hash256 manifest_hash;
  Hash256 body_hash;
  uint64_t event_count = 0;
  uint32_t body_size_bytes = 0;
  std::string projected_memory;
};

absl::StatusOr<ProjectionCheckpoint> CreateProjectionCheckpoint(
    const EventSourcedLog& log, DPMProjector* projector,
    const ProjectionCheckpointConfig& config, CheckpointStore* store,
    MerkleDagStore* dag);

absl::StatusOr<std::string> LoadProjectionCheckpoint(
    const DPMLogIdentity& identity, const Hash256& manifest_hash,
    CheckpointStore* store);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_CHECKPOINTED_PROJECTION_H_
