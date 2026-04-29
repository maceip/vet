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

#include "runtime/dpm/checkpointed_projection.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "runtime/dpm/dpm_projector.h"
#include "runtime/dpm/event.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/platform/checkpoint/canonical_manifest.h"
#include "runtime/platform/checkpoint/checkpoint_store.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/platform/provenance/merkle_dag_store.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {
namespace {

bool IsZeroHash(const Hash256& hash) {
  static const Hash256 kZero;
  return hash == kZero;
}

absl::Status ValidateConfig(const ProjectionCheckpointConfig& config) {
  if (config.projection.model_id.empty()) {
    return absl::InvalidArgumentError(
        "ProjectionCheckpointConfig requires projection.model_id.");
  }
  if (config.projection.schema_id.empty()) {
    return absl::InvalidArgumentError(
        "ProjectionCheckpointConfig requires projection.schema_id.");
  }
  if (config.projection.schema_json.empty()) {
    return absl::InvalidArgumentError(
        "ProjectionCheckpointConfig requires projection.schema_json.");
  }
  if (config.branch_id.empty()) {
    return absl::InvalidArgumentError(
        "ProjectionCheckpointConfig requires branch_id.");
  }
  if (IsZeroHash(config.model_artifact_hash)) {
    return absl::InvalidArgumentError(
        "ProjectionCheckpointConfig requires model_artifact_hash.");
  }
  if (config.created_unix_micros <= 0) {
    return absl::InvalidArgumentError(
        "ProjectionCheckpointConfig requires created_unix_micros.");
  }
  return absl::OkStatus();
}

CanonicalManifestInput BuildManifestInput(
    const EventSourcedLog& log, const ProjectionCheckpointConfig& config,
    uint64_t event_count, const Hash256& body_hash, uint32_t body_size_bytes) {
  CanonicalManifestInput input;
  input.tenant_id = log.identity().tenant_id;
  input.session_id = log.identity().session_id;
  input.branch_id = config.branch_id;
  input.level = config.level;
  input.compaction_interval = config.compaction_interval;
  input.parent_hashes = config.parent_manifest_hashes;
  input.architecture_tag = config.architecture_tag;
  input.producer_id = config.producer_id;
  input.runtime_version = config.runtime_version;
  input.model_artifact_hash = config.model_artifact_hash;
  input.model_id = config.projection.model_id;
  input.model_class = config.model_class;
  input.num_layers = config.num_layers;
  input.num_kv_heads = config.num_kv_heads;
  input.head_dim = config.head_dim;
  input.kv_dtype = config.kv_dtype;
  input.base_event_index = event_count;
  input.body_hash = body_hash;
  input.body_size_bytes = body_size_bytes;
  input.created_unix_micros = config.created_unix_micros;
  return input;
}

}  // namespace

absl::StatusOr<ProjectionCheckpoint> CreateProjectionCheckpoint(
    const EventSourcedLog& log, DPMProjector* projector,
    const ProjectionCheckpointConfig& config, CheckpointStore* store,
    MerkleDagStore* dag) {
  if (projector == nullptr) {
    return absl::InvalidArgumentError("DPM projector is required.");
  }
  if (store == nullptr) {
    return absl::InvalidArgumentError("CheckpointStore is required.");
  }
  if (dag == nullptr) {
    return absl::InvalidArgumentError("MerkleDagStore is required.");
  }
  RETURN_IF_ERROR(ValidateConfig(config));

  ASSIGN_OR_RETURN(std::vector<Event> events, log.GetAllEvents());
  ASSIGN_OR_RETURN(std::string projected_memory,
                   projector->Project(log, config.projection));
  ASSIGN_OR_RETURN(Hash256 body_hash,
                   store->PutPayload(log.identity().tenant_id,
                                     log.identity().session_id,
                                     projected_memory, HashAlgorithm::kBlake3));
  const uint32_t body_size_bytes =
      static_cast<uint32_t>(projected_memory.size());
  CanonicalManifestInput manifest_input =
      BuildManifestInput(log, config, events.size(), body_hash,
                         body_size_bytes);
  ASSIGN_OR_RETURN(Hash256 manifest_hash,
                   ComputeManifestHash(HashAlgorithm::kBlake3,
                                       manifest_input));
  ASSIGN_OR_RETURN(std::string abi_bytes,
                   EncodeCanonicalManifest(manifest_input));
  RETURN_IF_ERROR(store->PutManifest(log.identity().tenant_id,
                                     log.identity().session_id, manifest_hash,
                                     abi_bytes, body_hash));
  RETURN_IF_ERROR(dag->Put(
      log.identity().tenant_id, log.identity().session_id,
      MerkleDagNode{
          .hash = manifest_hash,
          .parent_hashes = config.parent_manifest_hashes,
          .created_unix_micros = config.created_unix_micros,
          .annotations = absl::StrCat("projection_checkpoint;branch=",
                                      config.branch_id,
                                      ";base_event_index=", events.size()),
      }));

  return ProjectionCheckpoint{
      .manifest_hash = manifest_hash,
      .body_hash = body_hash,
      .event_count = static_cast<uint64_t>(events.size()),
      .body_size_bytes = body_size_bytes,
      .projected_memory = std::move(projected_memory),
  };
}

absl::StatusOr<std::string> LoadProjectionCheckpoint(
    const DPMLogIdentity& identity, const Hash256& manifest_hash,
    CheckpointStore* store) {
  if (store == nullptr) {
    return absl::InvalidArgumentError("CheckpointStore is required.");
  }
  ASSIGN_OR_RETURN(CheckpointStore::ManifestRecord manifest,
                   store->GetManifest(identity.tenant_id, identity.session_id,
                                      manifest_hash));
  ASSIGN_OR_RETURN(std::string payload,
                   store->GetPayload(identity.tenant_id, identity.session_id,
                                     manifest.referenced_body_hash));
  const Hash256 actual_body_hash = HashBytes(HashAlgorithm::kBlake3, payload);
  if (!(actual_body_hash == manifest.referenced_body_hash)) {
    return absl::DataLossError(
        "Projection checkpoint payload hash mismatch.");
  }
  return payload;
}

}  // namespace litert::lm
