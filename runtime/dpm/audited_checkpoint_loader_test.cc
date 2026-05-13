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

#include "runtime/dpm/audited_checkpoint_loader.h"

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
using ::testing::Not;

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
    if (next_ >= responses_.size()) return responses_.back();
    return responses_[next_++];
  }

  std::vector<std::string> prompts;

 private:
  std::vector<std::string> responses_;
  int next_ = 0;
};

ProjectionCheckpointConfig CheckpointConfig() {
  ProjectionCheckpointConfig config;
  config.projection.schema_id = "incident_response_v1";
  config.projection.schema_json =
      R"json({"Facts":["string with [i]"],"Reasoning":["string with [i]"],"Compliance":["string with [i]"]})json";
  config.projection.model_id = "pinned-test-model";
  config.projection.memory_budget_chars = 1338;
  config.model_artifact_hash = HashBytes(HashAlgorithm::kBlake3, "model");
  config.architecture_tag = "x86_64-cpu";
  config.producer_id = "audited_checkpoint_loader_test";
  config.runtime_version = "test";
  config.created_unix_micros = 1777390000000000;
  return config;
}

AuditCertificate CertificateFor(const ProjectionCheckpoint& checkpoint,
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
  certificate.auditor_model_id = "audited-loader-test";
  certificate.audit_policy_version = "exact-replay-v1";
  certificate.verdict = verdict;
  certificate.drift_score = drift_score;
  certificate.provenance_root_hash = checkpoint.manifest_hash;
  certificate.created_unix_micros = created_unix_micros;
  return certificate;
}

AuditedProjectionCheckpointRequest RequestFor(
    const EventSourcedLog& log, const ProjectionCheckpoint& checkpoint) {
  return AuditedProjectionCheckpointRequest{
      .identity = log.identity(),
      .checkpoint_manifest_hash = checkpoint.manifest_hash,
      .checkpoint_event_count = checkpoint.event_count,
      .compatibility_ok = true,
      .thaw_verification_ok = true,
  };
}

TEST(AuditedCheckpointLoaderTest,
     LoadsProjectionOnlyAfterPassingAuditGate) {
  EventSourcedLog log(TestRoot("audited_loader_log"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "auditd shows T1021",
      .timestamp_us = 1,
  }));

  const std::string projection =
      R"json({"Facts":["T1021 [1]"],"Reasoning":["lateral movement [1]"],"Compliance":["audit retained [1]"]})json";
  RecordingRunner runner({projection});
  DPMProjector projector(&runner);
  const std::filesystem::path root = TestRoot("audited_loader_store");
  LocalFilesystemCheckpointStore checkpoint_store(root);
  LocalFilesystemAuditLedger ledger(root);
  LocalMerkleDagStore dag(root);
  ProjectionCheckpointConfig config = CheckpointConfig();
  ASSERT_OK_AND_ASSIGN(
      ProjectionCheckpoint checkpoint,
      CreateProjectionCheckpoint(log, &projector, config, &checkpoint_store,
                                 &dag));

  ASSERT_OK_AND_ASSIGN(AuditCertificate pass,
                       FinalizeAuditCertificate(CertificateFor(
                           checkpoint, config, AuditVerdict::kPass, 0.0,
                           1777390000000010)));
  ASSERT_OK(ledger.PutCertificate(pass));
  ASSERT_OK_AND_ASSIGN(std::vector<Event> events, log.GetAllEvents());
  ASSERT_OK_AND_ASSIGN(CorrectionIndex corrections,
                       CorrectionIndex::Build(events));

  ASSERT_OK_AND_ASSIGN(
      AuditedProjectionCheckpoint loaded,
      LoadAuditedProjectionCheckpointForDecision(
          RequestFor(log, checkpoint), &checkpoint_store, ledger,
          corrections));
  EXPECT_EQ(loaded.projected_memory, projection);
  EXPECT_TRUE(loaded.gate.may_use);
}

TEST(AuditedCheckpointLoaderTest,
     RejectsMissingAuditAndBlockingCorrectionBeforePayloadLoad) {
  EventSourcedLog log(TestRoot("audited_loader_reject_log"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "auditd shows T1021",
      .timestamp_us = 1,
  }));

  RecordingRunner runner({
      R"json({"Facts":["T1021 [1]"],"Reasoning":["lateral movement [1]"],"Compliance":["audit retained [1]"]})json",
  });
  DPMProjector projector(&runner);
  const std::filesystem::path root = TestRoot("audited_loader_reject_store");
  LocalFilesystemCheckpointStore checkpoint_store(root);
  LocalFilesystemAuditLedger ledger(root);
  LocalMerkleDagStore dag(root);
  ProjectionCheckpointConfig config = CheckpointConfig();
  ASSERT_OK_AND_ASSIGN(
      ProjectionCheckpoint checkpoint,
      CreateProjectionCheckpoint(log, &projector, config, &checkpoint_store,
                                 &dag));
  ASSERT_OK_AND_ASSIGN(std::vector<Event> events, log.GetAllEvents());
  ASSERT_OK_AND_ASSIGN(CorrectionIndex clean_corrections,
                       CorrectionIndex::Build(events));

  absl::StatusOr<AuditedProjectionCheckpoint> missing_audit =
      LoadAuditedProjectionCheckpointForDecision(
          RequestFor(log, checkpoint), &checkpoint_store, ledger,
          clean_corrections);
  ASSERT_FALSE(missing_audit.ok());
  EXPECT_THAT(std::string(missing_audit.status().message()),
              HasSubstr("no audit certificate"));

  ASSERT_OK_AND_ASSIGN(AuditCertificate pass,
                       FinalizeAuditCertificate(CertificateFor(
                           checkpoint, config, AuditVerdict::kPass, 0.0,
                           1777390000000010)));
  ASSERT_OK(ledger.PutCertificate(pass));
  CorrectionPayload correction;
  correction.correction_id = "corr-1";
  correction.target_checkpoint_manifest_hash = checkpoint.manifest_hash;
  correction.target_event_range_start = 0;
  correction.target_event_range_end = checkpoint.event_count;
  correction.audit_certificate_id = pass.certificate_id;
  correction.reason_code = "projection_drift";
  correction.severity = CorrectionSeverity::kBlocking;
  correction.invalidates_checkpoints = {checkpoint.manifest_hash};
  correction.must_interrupt_before_next_predict = true;
  correction.created_unix_micros = 1777390000000020;
  ASSERT_OK(AppendCorrectionEvent(&log, correction));
  ASSERT_OK_AND_ASSIGN(events, log.GetAllEvents());
  ASSERT_OK_AND_ASSIGN(CorrectionIndex blocking_corrections,
                       CorrectionIndex::Build(events));

  absl::StatusOr<AuditedProjectionCheckpoint> blocked =
      LoadAuditedProjectionCheckpointForDecision(
          RequestFor(log, checkpoint), &checkpoint_store, ledger,
          blocking_corrections);
  ASSERT_FALSE(blocked.ok());
  EXPECT_THAT(std::string(blocked.status().message()),
              HasSubstr("blocking correction"));
}

TEST(AuditedCheckpointLoaderTest,
     BlockingCorrectionReplaysRawLogWithCorrectionDirectives) {
  EventSourcedLog log(TestRoot("audited_loader_replay_log"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "initial analysis says transport as main result",
      .timestamp_us = 1,
  }));

  const std::string stale_projection =
      R"json({"Facts":["transport as main result [1]"],"Reasoning":["old path [1]"],"Compliance":["audit retained [1]"]})json";
  RecordingRunner checkpoint_runner({stale_projection});
  DPMProjector checkpoint_projector(&checkpoint_runner);
  const std::filesystem::path root = TestRoot("audited_loader_replay_store");
  LocalFilesystemCheckpointStore checkpoint_store(root);
  LocalFilesystemAuditLedger ledger(root);
  LocalMerkleDagStore dag(root);
  ProjectionCheckpointConfig config = CheckpointConfig();
  ASSERT_OK_AND_ASSIGN(
      ProjectionCheckpoint checkpoint,
      CreateProjectionCheckpoint(log, &checkpoint_projector, config,
                                 &checkpoint_store, &dag));
  ASSERT_OK_AND_ASSIGN(AuditCertificate pass,
                       FinalizeAuditCertificate(CertificateFor(
                           checkpoint, config, AuditVerdict::kPass, 0.0,
                           1777390000000010)));
  ASSERT_OK(ledger.PutCertificate(pass));

  CorrectionPayload correction;
  correction.correction_id = "corr-transport";
  correction.target_checkpoint_manifest_hash = checkpoint.manifest_hash;
  correction.target_event_range_start = 0;
  correction.target_event_range_end = checkpoint.event_count;
  correction.audit_certificate_id = pass.certificate_id;
  correction.reason_code = "projection_drift";
  correction.severity = CorrectionSeverity::kBlocking;
  correction.invalidates_checkpoints = {checkpoint.manifest_hash};
  correction.correction_text = "Transport was not the main result.";
  correction.invalidated_facts = {"transport as main result"};
  correction.replacement_facts = {"credential theft is the main result [2]"};
  correction.must_interrupt_before_next_predict = true;
  correction.created_unix_micros = 1777390000000020;
  ASSERT_OK(AppendCorrectionEvent(&log, correction));

  ASSERT_OK_AND_ASSIGN(std::vector<Event> events, log.GetAllEvents());
  ASSERT_OK_AND_ASSIGN(CorrectionIndex corrections,
                       CorrectionIndex::Build(events));
  RecordingRunner replay_runner({
      R"json({"Facts":["credential theft is the main result [2]"],"Reasoning":["correction supersedes earlier analysis [2]"],"Compliance":["audit retained [2]"]})json",
  });
  DPMProjector replay_projector(&replay_runner);
  CorrectionAwareCheckpointReplayRequest replay_request;
  replay_request.checkpoint = RequestFor(log, checkpoint);
  replay_request.projection = config.projection;

  ASSERT_OK_AND_ASSIGN(
      CorrectionAwareCheckpointReplay replay,
      LoadOrReplayAuditedProjectionCheckpointForDecision(
          log, &replay_projector, replay_request, &checkpoint_store, ledger,
          corrections));

  EXPECT_FALSE(replay.gate.may_use);
  EXPECT_TRUE(replay.replayed_from_raw);
  ASSERT_EQ(replay.correction_directives.size(), 1);
  EXPECT_EQ(replay.correction_directives[0].invalidated_facts,
            correction.invalidated_facts);
  EXPECT_EQ(replay.active_evidence_view.event_range_start, 0);
  EXPECT_EQ(replay.active_evidence_view.event_range_end, 2);
  ASSERT_EQ(replay.active_evidence_view.revoked_records.size(), 1);
  EXPECT_EQ(replay.active_evidence_view.revoked_records[0].global_event_index,
            0);
  EXPECT_THAT(
      replay.active_evidence_view.active_event_log,
      HasSubstr("REVOKED_BY_CORRECTION"));
  EXPECT_THAT(
      replay.active_evidence_view.active_event_log,
      Not(HasSubstr("initial analysis says transport as main result")));
  EXPECT_THAT(
      replay.active_evidence_view.revoked_evidence_log,
      HasSubstr("initial analysis says transport as main result"));
  EXPECT_THAT(replay.projected_memory,
              HasSubstr("credential theft is the main result"));
  EXPECT_THAT(replay.projected_memory,
              Not(HasSubstr("transport as main result")));
  ASSERT_EQ(replay_runner.prompts.size(), 1);
  EXPECT_THAT(replay_runner.prompts[0], HasSubstr("[BLOCKING CORRECTIONS]"));
  EXPECT_THAT(replay_runner.prompts[0], HasSubstr("corr-transport"));
}

}  // namespace
}  // namespace litert::lm
