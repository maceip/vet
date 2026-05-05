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

#include "runtime/dpm/checkpoint_decision_gate.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "runtime/dpm/checkpointed_projection.h"
#include "runtime/dpm/correction_protocol.h"
#include "runtime/dpm/dpm_projector.h"
#include "runtime/dpm/event.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/platform/audit/audit_certificate.h"
#include "runtime/platform/audit/local_filesystem_audit_ledger.h"
#include "runtime/platform/checkpoint/local_filesystem_checkpoint_store.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/platform/provenance/local_merkle_dag_store.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

using ::testing::HasSubstr;

std::filesystem::path TestRoot(absl::string_view name) {
  std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / std::string(name);
  std::filesystem::remove_all(path);
  return path;
}

class RecordingRunner : public DPMInferenceRunner {
 public:
  explicit RecordingRunner(std::vector<std::string> responses)
      : responses_(std::move(responses)) {}

  absl::StatusOr<std::string> Generate(
      absl::string_view prompt, const DPMInferenceConfig& config) override {
    prompts.push_back(std::string(prompt));
    configs.push_back(config);
    if (next_ >= responses_.size()) {
      return responses_.back();
    }
    return responses_[next_++];
  }

  std::vector<std::string> prompts;
  std::vector<DPMInferenceConfig> configs;

 private:
  std::vector<std::string> responses_;
  int next_ = 0;
};

ProjectionCheckpointConfig BaseCheckpointConfig() {
  ProjectionCheckpointConfig config;
  config.projection.schema_id = "incident_response_v1";
  config.projection.schema_json =
      R"json({"Facts":["string with [i]"],"Reasoning":["string with [i]"],"Compliance":["string with [i]"]})json";
  config.projection.model_id = "pinned-test-model";
  config.projection.memory_budget_chars = 1338;
  config.model_artifact_hash = HashBytes(HashAlgorithm::kBlake3, "model");
  config.architecture_tag = "x86_64-cpu";
  config.producer_id = "phase3_audit_flow_test";
  config.runtime_version = "test";
  config.created_unix_micros = 1777390000000000;
  return config;
}

AuditCertificate BaseCertificate(const ProjectionCheckpoint& checkpoint,
                                 const ProjectionCheckpointConfig& config,
                                 AuditVerdict verdict, double drift_score,
                                 int64_t created_unix_micros) {
  AuditCertificate certificate;
  certificate.checkpoint_manifest_hash = checkpoint.manifest_hash;
  certificate.checkpoint_body_hash = checkpoint.body_hash;
  certificate.tenant_id = "tenant-a";
  certificate.session_id = "session-1";
  certificate.branch_id = config.branch_id;
  certificate.event_range_start = 0;
  certificate.event_range_end = checkpoint.event_count;
  certificate.log_generation = checkpoint.event_count;
  certificate.schema_id = config.projection.schema_id;
  certificate.model_artifact_hash = config.model_artifact_hash;
  certificate.projection_model_id = config.projection.model_id;
  certificate.auditor_model_id = "exact-projection-comparator";
  certificate.audit_policy_version = "exact-v1";
  certificate.verdict = verdict;
  certificate.drift_score = drift_score;
  certificate.provenance_root_hash = checkpoint.manifest_hash;
  certificate.created_unix_micros = created_unix_micros;
  return certificate;
}

CheckpointDecisionGateRequest GateRequest(
    const DPMLogIdentity& identity, const ProjectionCheckpoint& checkpoint) {
  return CheckpointDecisionGateRequest{
      .identity = identity,
      .checkpoint_manifest_hash = checkpoint.manifest_hash,
      .checkpoint_event_count = checkpoint.event_count,
      .compatibility_ok = true,
      .thaw_verification_ok = true,
  };
}

TEST(Phase3AuditFlowTest,
     PassCertificateAllowsDecisionCorrectionInvalidatesOldCheckpoint) {
  EventSourcedLog log(TestRoot("phase3_audit_log"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .payload = "analyst asks for incident stage",
      .timestamp_us = 1,
  }));
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "auditd shows T1021 lateral movement",
      .timestamp_us = 2,
  }));

  RecordingRunner runner({
      R"json({"Facts":["T1021 lateral movement [2]"],"Reasoning":["stage escalated [2]"],"Compliance":["audit trail retained [1]"]})json",
      R"json({"Facts":["T1021 lateral movement [2]","correction invalidated prior checkpoint [3]"],"Reasoning":["fresh projection after correction [3]"],"Compliance":["audit trail retained [1]"]})json",
  });
  DPMProjector projector(&runner);
  const std::filesystem::path root = TestRoot("phase3_audit_store");
  LocalFilesystemCheckpointStore checkpoint_store(root);
  LocalMerkleDagStore dag_store(root);
  LocalFilesystemAuditLedger ledger(root);

  ProjectionCheckpointConfig config = BaseCheckpointConfig();
  ASSERT_OK_AND_ASSIGN(
      ProjectionCheckpoint checkpoint,
      CreateProjectionCheckpoint(log, &projector, config, &checkpoint_store,
                                 &dag_store));
  ASSERT_OK_AND_ASSIGN(AuditCertificate pass_certificate,
                       FinalizeAuditCertificate(BaseCertificate(
                           checkpoint, config, AuditVerdict::kPass, 0.0,
                           1777390000000010)));
  ASSERT_OK(ledger.PutCertificate(pass_certificate));

  ASSERT_OK_AND_ASSIGN(std::vector<Event> events_before_correction,
                       log.GetAllEvents());
  ASSERT_OK_AND_ASSIGN(CorrectionIndex clean_index,
                       CorrectionIndex::Build(events_before_correction));
  ASSERT_OK_AND_ASSIGN(
      CheckpointDecisionGateResult usable,
      MayUseCheckpointForDecision(GateRequest(log.identity(), checkpoint),
                                  ledger, clean_index));
  EXPECT_TRUE(usable.may_use);

  AuditCertificate drift_certificate = BaseCertificate(
      checkpoint, config, AuditVerdict::kCorrectionEmitted, 1.0,
      1777390000000020);
  drift_certificate.drift_fields = {"Facts[0]"};
  drift_certificate.correction_event_ids = {"corr-1"};
  ASSERT_OK_AND_ASSIGN(drift_certificate,
                       FinalizeAuditCertificate(drift_certificate));
  ASSERT_OK(ledger.PutCertificate(drift_certificate));

  CorrectionPayload correction;
  correction.correction_id = "corr-1";
  correction.target_checkpoint_manifest_hash = checkpoint.manifest_hash;
  correction.target_event_range_start = 0;
  correction.target_event_range_end = checkpoint.event_count;
  correction.audit_certificate_id = drift_certificate.certificate_id;
  correction.reason_code = "projection_drift";
  correction.severity = CorrectionSeverity::kBlocking;
  correction.drift_fields = {"Facts[0]"};
  correction.invalidates_checkpoints = {checkpoint.manifest_hash};
  correction.replacement_projection = "must re-project from corrected log";
  correction.must_interrupt_before_next_predict = true;
  correction.created_unix_micros = 1777390000000030;
  ASSERT_OK(AppendCorrectionEvent(&log, correction));

  ASSERT_OK_AND_ASSIGN(std::vector<Event> corrected_events,
                       log.GetAllEvents());
  ASSERT_OK_AND_ASSIGN(CorrectionIndex correction_index,
                       CorrectionIndex::Build(corrected_events));
  ASSERT_TRUE(correction_index.HasBlockingCorrectionFor(
      checkpoint.manifest_hash));
  ASSERT_OK_AND_ASSIGN(
      CheckpointDecisionGateResult rejected,
      MayUseCheckpointForDecision(GateRequest(log.identity(), checkpoint),
                                  ledger, correction_index));
  EXPECT_FALSE(rejected.may_use);
  EXPECT_THAT(rejected.reason, HasSubstr("blocking correction"));

  ProjectionCheckpointConfig fresh_config = config;
  fresh_config.parent_manifest_hashes = {checkpoint.manifest_hash};
  fresh_config.created_unix_micros = 1777390000000040;
  ASSERT_OK_AND_ASSIGN(
      ProjectionCheckpoint fresh_checkpoint,
      CreateProjectionCheckpoint(log, &projector, fresh_config,
                                 &checkpoint_store, &dag_store));
  ASSERT_OK_AND_ASSIGN(AuditCertificate fresh_pass,
                       FinalizeAuditCertificate(BaseCertificate(
                           fresh_checkpoint, fresh_config,
                           AuditVerdict::kPass, 0.0, 1777390000000050)));
  ASSERT_OK(ledger.PutCertificate(fresh_pass));

  ASSERT_OK_AND_ASSIGN(
      CheckpointDecisionGateResult fresh_usable,
      MayUseCheckpointForDecision(
          GateRequest(log.identity(), fresh_checkpoint), ledger,
          correction_index));
  EXPECT_TRUE(fresh_usable.may_use);
  EXPECT_THAT(fresh_checkpoint.projected_memory,
              HasSubstr("correction invalidated prior checkpoint"));
}

}  // namespace
}  // namespace litert::lm
