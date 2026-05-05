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

#include "runtime/dpm/projection_replay_auditor.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "runtime/dpm/checkpointed_projection.h"
#include "runtime/dpm/correction_protocol.h"
#include "runtime/dpm/dpm_projector.h"
#include "runtime/dpm/event.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/platform/audit/audit_certificate.h"
#include "runtime/platform/audit/audit_ledger.h"
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

absl::Status ValidateConfig(const ProjectionReplayAuditConfig& config) {
  if (config.checkpoint.projection.schema_id.empty() ||
      config.checkpoint.projection.model_id.empty() ||
      config.checkpoint.projection.schema_json.empty()) {
    return absl::InvalidArgumentError(
        "ProjectionReplayAuditConfig requires projection schema/model.");
  }
  if (config.checkpoint.branch_id.empty()) {
    return absl::InvalidArgumentError(
        "ProjectionReplayAuditConfig requires branch_id.");
  }
  if (IsZeroHash(config.checkpoint.model_artifact_hash)) {
    return absl::InvalidArgumentError(
        "ProjectionReplayAuditConfig requires model_artifact_hash.");
  }
  if (config.auditor_model_id.empty() ||
      config.audit_policy_version.empty()) {
    return absl::InvalidArgumentError(
        "ProjectionReplayAuditConfig requires auditor/policy identity.");
  }
  if (config.created_unix_micros <= 0) {
    return absl::InvalidArgumentError(
        "ProjectionReplayAuditConfig requires created_unix_micros.");
  }
  return absl::OkStatus();
}

std::string CorrectionIdFor(const Hash256& checkpoint_manifest_hash,
                            int64_t created_unix_micros) {
  const Hash256 hash = HashBytes(
      HashAlgorithm::kBlake3,
      absl::StrCat("correction:", checkpoint_manifest_hash.ToHex(), ":",
                   created_unix_micros));
  return absl::StrCat("corr-", hash.ToHex().substr(0, 32));
}

AuditCertificate BuildCertificate(
    const EventSourcedLog& log,
    const ProjectionReplayAuditConfig& config,
    const Hash256& checkpoint_manifest_hash,
    const Hash256& checkpoint_body_hash,
    uint64_t event_count,
    AuditVerdict verdict,
    double drift_score,
    std::vector<std::string> drift_fields,
    std::vector<std::string> correction_event_ids) {
  AuditCertificate certificate;
  certificate.checkpoint_manifest_hash = checkpoint_manifest_hash;
  certificate.checkpoint_body_hash = checkpoint_body_hash;
  certificate.tenant_id = log.identity().tenant_id;
  certificate.session_id = log.identity().session_id;
  certificate.branch_id = config.checkpoint.branch_id;
  certificate.event_range_start = 0;
  certificate.event_range_end = event_count;
  certificate.log_generation = event_count;
  certificate.schema_id = config.checkpoint.projection.schema_id;
  certificate.model_artifact_hash = config.checkpoint.model_artifact_hash;
  certificate.projection_model_id = config.checkpoint.projection.model_id;
  certificate.auditor_model_id = config.auditor_model_id;
  certificate.audit_policy_version = config.audit_policy_version;
  certificate.verdict = verdict;
  certificate.drift_score = drift_score;
  certificate.drift_fields = std::move(drift_fields);
  certificate.correction_event_ids = std::move(correction_event_ids);
  certificate.provenance_root_hash = checkpoint_manifest_hash;
  certificate.created_unix_micros = config.created_unix_micros;
  return certificate;
}

absl::Status LinkAuditCertificateToDag(
    const EventSourcedLog& log, const AuditCertificate& certificate,
    MerkleDagStore* dag) {
  return dag->Put(
      log.identity().tenant_id, log.identity().session_id,
      MerkleDagNode{
          .hash = certificate.certificate_id,
          .parent_hashes = {certificate.checkpoint_manifest_hash},
          .created_unix_micros = certificate.created_unix_micros,
          .annotations = absl::StrCat(
              "audit_certificate;checkpoint_manifest_hash=",
              certificate.checkpoint_manifest_hash.ToHex(), ";verdict=",
              AuditVerdictToString(certificate.verdict), ";policy=",
              certificate.audit_policy_version),
      });
}

}  // namespace

absl::StatusOr<ProjectionReplayAuditResult> VerifyProjectionCheckpointFromRaw(
    const EventSourcedLog& log, DPMProjector* projector,
    const Hash256& checkpoint_manifest_hash,
    const ProjectionReplayAuditConfig& config, CheckpointStore* store,
    AuditLedger* ledger, MerkleDagStore* dag) {
  if (projector == nullptr) {
    return absl::InvalidArgumentError("DPM projector is required.");
  }
  if (store == nullptr) {
    return absl::InvalidArgumentError("CheckpointStore is required.");
  }
  if (ledger == nullptr) {
    return absl::InvalidArgumentError("AuditLedger is required.");
  }
  if (dag == nullptr) {
    return absl::InvalidArgumentError("MerkleDagStore is required.");
  }
  if (IsZeroHash(checkpoint_manifest_hash)) {
    return absl::InvalidArgumentError(
        "VerifyProjectionCheckpointFromRaw requires checkpoint_manifest_hash.");
  }
  RETURN_IF_ERROR(ValidateConfig(config));

  ASSIGN_OR_RETURN(CheckpointStore::ManifestRecord manifest,
                   store->GetManifest(log.identity().tenant_id,
                                      log.identity().session_id,
                                      checkpoint_manifest_hash));
  ASSIGN_OR_RETURN(std::string stored_projection,
                   store->GetPayload(log.identity().tenant_id,
                                     log.identity().session_id,
                                     manifest.referenced_body_hash));
  const Hash256 stored_body_hash =
      HashBytes(HashAlgorithm::kBlake3, stored_projection);
  if (stored_body_hash != manifest.referenced_body_hash) {
    return absl::DataLossError(
        "stored checkpoint payload does not match manifest body hash.");
  }

  ASSIGN_OR_RETURN(std::vector<Event> events, log.GetAllEvents());
  ASSIGN_OR_RETURN(std::string replayed_projection,
                   projector->Project(log, config.checkpoint.projection));
  const Hash256 replayed_body_hash =
      HashBytes(HashAlgorithm::kBlake3, replayed_projection);
  const bool matched = replayed_body_hash == manifest.referenced_body_hash;

  std::optional<CorrectionPayload> correction;
  std::vector<std::string> correction_event_ids;
  AuditVerdict verdict = AuditVerdict::kPass;
  double drift_score = 0.0;
  std::vector<std::string> drift_fields;
  if (!matched) {
    verdict = AuditVerdict::kCorrectionEmitted;
    drift_score = 1.0;
    drift_fields = {"projected_memory_hash"};
    if (config.emit_correction_on_drift) {
      correction_event_ids.push_back(
          CorrectionIdFor(checkpoint_manifest_hash,
                          config.created_unix_micros));
    }
  }

  ASSIGN_OR_RETURN(AuditCertificate certificate,
                   FinalizeAuditCertificate(BuildCertificate(
                       log, config, checkpoint_manifest_hash,
                       manifest.referenced_body_hash, events.size(), verdict,
                       drift_score, drift_fields, correction_event_ids)));

  if (!matched && config.emit_correction_on_drift) {
    CorrectionPayload payload;
    payload.correction_id = certificate.correction_event_ids.front();
    payload.target_checkpoint_manifest_hash = checkpoint_manifest_hash;
    payload.target_event_range_start = certificate.event_range_start;
    payload.target_event_range_end = certificate.event_range_end;
    payload.audit_certificate_id = certificate.certificate_id;
    payload.reason_code = "projection_replay_mismatch";
    payload.severity = CorrectionSeverity::kBlocking;
    payload.drift_fields = certificate.drift_fields;
    payload.invalidates_checkpoints = {checkpoint_manifest_hash};
    payload.replacement_projection = replayed_projection;
    payload.must_interrupt_before_next_predict = true;
    payload.created_unix_micros = config.created_unix_micros;
    correction = std::move(payload);
  }

  RETURN_IF_ERROR(ledger->PutCertificate(certificate));
  RETURN_IF_ERROR(LinkAuditCertificateToDag(log, certificate, dag));

  return ProjectionReplayAuditResult{
      .certificate = std::move(certificate),
      .correction = std::move(correction),
      .replayed_projection = std::move(replayed_projection),
  };
}

}  // namespace litert::lm
