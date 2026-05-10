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

#include <fstream>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "runtime/dpm/checkpoint_decision_gate.h"
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
#include "runtime/platform/provenance/provenance_query.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

using ::testing::HasSubstr;
using ::testing::UnorderedElementsAre;

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
    if (next_ >= responses_.size()) return responses_.back();
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
  config.producer_id = "projection_replay_auditor_test";
  config.runtime_version = "test";
  config.created_unix_micros = 1777390000000000;
  return config;
}

ProjectionReplayAuditConfig AuditConfig(ProjectionCheckpointConfig checkpoint) {
  ProjectionReplayAuditConfig config;
  config.checkpoint = std::move(checkpoint);
  config.auditor_model_id = "exact-replay-test";
  config.audit_policy_version = "exact-replay-v1";
  config.created_unix_micros = 1777390000000100;
  return config;
}

CorrectionPayload BlockingCorrectionFor(const Hash256& target,
                                        const Hash256& certificate_id) {
  CorrectionPayload correction;
  correction.correction_id = "corr-other";
  correction.target_checkpoint_manifest_hash = target;
  correction.target_event_range_start = 0;
  correction.target_event_range_end = 1;
  correction.audit_certificate_id = certificate_id;
  correction.reason_code = "projection_replay_mismatch";
  correction.severity = CorrectionSeverity::kBlocking;
  correction.invalidates_checkpoints = {target};
  correction.must_interrupt_before_next_predict = true;
  correction.created_unix_micros = 1777390000000999;
  return correction;
}

TEST(ProjectionReplayAuditorTest,
     MatchingReplayWritesPassCertificateAndDagLink) {
  EventSourcedLog log(TestRoot("audit_replay_pass_log"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "auditd shows T1021 lateral movement",
      .timestamp_us = 1,
  }));

  const std::string projection =
      R"json({"Facts":["T1021 lateral movement [1]"],"Reasoning":["stage escalated [1]"],"Compliance":["audit trail retained [1]"]})json";
  RecordingRunner checkpoint_runner({projection});
  DPMProjector checkpoint_projector(&checkpoint_runner);
  const std::filesystem::path root = TestRoot("audit_replay_pass_store");
  LocalFilesystemCheckpointStore checkpoint_store(root);
  LocalFilesystemAuditLedger ledger(root);
  LocalMerkleDagStore dag(root);
  ProjectionCheckpointConfig checkpoint_config = BaseCheckpointConfig();
  ASSERT_OK_AND_ASSIGN(
      ProjectionCheckpoint checkpoint,
      CreateProjectionCheckpoint(log, &checkpoint_projector,
                                 checkpoint_config, &checkpoint_store, &dag));

  RecordingRunner audit_runner({projection});
  DPMProjector audit_projector(&audit_runner);
  ASSERT_OK_AND_ASSIGN(
      ProjectionReplayAuditResult audit,
      VerifyProjectionCheckpointFromRaw(
          log, &audit_projector, checkpoint.manifest_hash,
          AuditConfig(checkpoint_config), &checkpoint_store, &ledger, &dag));

  EXPECT_EQ(audit.certificate.verdict, AuditVerdict::kPass);
  EXPECT_EQ(audit.certificate.drift_score, 0.0);
  EXPECT_EQ(audit.certificate.event_range_start, 0);
  EXPECT_EQ(audit.certificate.event_range_end, checkpoint.event_count);
  EXPECT_EQ(audit.certificate.log_generation, checkpoint.event_count);
  EXPECT_FALSE(audit.correction.has_value());
  ASSERT_OK_AND_ASSIGN(
      AuditCertificate stored,
      ledger.LatestForCheckpoint("tenant-a", "session-1",
                                 checkpoint.manifest_hash));
  EXPECT_EQ(stored.certificate_id, audit.certificate.certificate_id);

  ASSERT_OK_AND_ASSIGN(
      ProvenanceChain chain,
      GetCheckpointProvenance(dag, "tenant-a", "session-1",
                              audit.certificate.certificate_id));
  ASSERT_EQ(chain.nodes.size(), 2);
  EXPECT_EQ(chain.nodes[0].hash, audit.certificate.certificate_id);
  EXPECT_EQ(chain.nodes[1].hash, checkpoint.manifest_hash);
  EXPECT_THAT(chain.nodes[0].annotations, HasSubstr("audit_certificate"));
}

TEST(ProjectionReplayAuditorTest, BodyTamperFailsBeforeReplayCertificate) {
  EventSourcedLog log(TestRoot("audit_replay_tamper_log"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-tamper",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "auditd shows T1021 lateral movement",
      .timestamp_us = 1,
  }));

  const std::string projection =
      R"json({"Facts":["T1021 lateral movement [1]"],"Reasoning":["stage escalated [1]"],"Compliance":["audit trail retained [1]"]})json";
  RecordingRunner checkpoint_runner({projection});
  DPMProjector checkpoint_projector(&checkpoint_runner);
  const std::filesystem::path root = TestRoot("audit_replay_tamper_store");
  LocalFilesystemCheckpointStore checkpoint_store(root);
  LocalFilesystemAuditLedger ledger(root);
  LocalMerkleDagStore dag(root);
  ProjectionCheckpointConfig checkpoint_config = BaseCheckpointConfig();
  ASSERT_OK_AND_ASSIGN(
      ProjectionCheckpoint checkpoint,
      CreateProjectionCheckpoint(log, &checkpoint_projector,
                                 checkpoint_config, &checkpoint_store, &dag));

  std::fstream payload(
      checkpoint_store.PayloadPathFor("tenant-a", "session-tamper",
                                      checkpoint.body_hash),
      std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(payload.is_open());
  payload.seekp(-1, std::ios::end);
  payload.put('X');
  payload.close();

  RecordingRunner audit_runner({projection});
  DPMProjector audit_projector(&audit_runner);
  absl::StatusOr<ProjectionReplayAuditResult> audit =
      VerifyProjectionCheckpointFromRaw(
          log, &audit_projector, checkpoint.manifest_hash,
          AuditConfig(checkpoint_config), &checkpoint_store, &ledger, &dag);
  ASSERT_FALSE(audit.ok());
  EXPECT_EQ(audit.status().code(), absl::StatusCode::kDataLoss);
  EXPECT_THAT(std::string(audit.status().message()),
              HasSubstr("payload hash mismatch"));
}

TEST(ProjectionReplayAuditorTest,
     CorrectionPayloadRoundTripsCorrectionAwareReplayFields) {
  const Hash256 checkpoint = HashBytes(HashAlgorithm::kBlake3, "checkpoint");
  const Hash256 certificate = HashBytes(HashAlgorithm::kBlake3, "certificate");
  CorrectionPayload payload = BlockingCorrectionFor(checkpoint, certificate);
  payload.correction_text = "Transport was not the main result.";
  payload.invalidated_facts = {"transport as main result"};
  payload.replacement_facts = {"credential theft is the main result [2]"};
  payload.scope = ProjectionCorrectionScope::kGlobal;

  ASSERT_OK_AND_ASSIGN(
      CorrectionPayload decoded,
      CorrectionPayloadFromJson(CorrectionPayloadToJson(payload)));
  EXPECT_EQ(decoded.correction_text, payload.correction_text);
  EXPECT_EQ(decoded.invalidated_facts, payload.invalidated_facts);
  EXPECT_EQ(decoded.replacement_facts, payload.replacement_facts);
  EXPECT_EQ(decoded.scope, ProjectionCorrectionScope::kGlobal);

  const std::vector<ProjectionCorrectionDirective> directives =
      BuildProjectionCorrectionDirectives({decoded});
  ASSERT_EQ(directives.size(), 1);
  EXPECT_EQ(directives[0].correction_event_id, payload.correction_id);
  EXPECT_EQ(directives[0].correction_text, payload.correction_text);
  EXPECT_EQ(directives[0].invalidated_facts, payload.invalidated_facts);
  EXPECT_EQ(directives[0].replacement_facts, payload.replacement_facts);
}

TEST(ProjectionReplayAuditorTest,
     DriftWritesCorrectionCertificateAndCorrectionPayloadTripsBarrier) {
  EventSourcedLog log(TestRoot("audit_replay_drift_log"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-2",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "auditd initially classified as STEP_2",
      .timestamp_us = 1,
  }));

  RecordingRunner checkpoint_runner({
      R"json({"Facts":["STEP_2 [1]"],"Reasoning":["initial projection [1]"],"Compliance":["audit retained [1]"]})json",
  });
  DPMProjector checkpoint_projector(&checkpoint_runner);
  const std::filesystem::path root = TestRoot("audit_replay_drift_store");
  LocalFilesystemCheckpointStore checkpoint_store(root);
  LocalFilesystemAuditLedger ledger(root);
  LocalMerkleDagStore dag(root);
  ProjectionCheckpointConfig checkpoint_config = BaseCheckpointConfig();
  ASSERT_OK_AND_ASSIGN(
      ProjectionCheckpoint checkpoint,
      CreateProjectionCheckpoint(log, &checkpoint_projector,
                                 checkpoint_config, &checkpoint_store, &dag));

  RecordingRunner audit_runner({
      R"json({"Facts":["STEP_4 [1]"],"Reasoning":["replay caught escalation [1]"],"Compliance":["audit retained [1]"]})json",
  });
  DPMProjector audit_projector(&audit_runner);
  ProjectionReplayAuditConfig audit_config = AuditConfig(checkpoint_config);
  audit_config.created_unix_micros += 1;
  ASSERT_OK_AND_ASSIGN(
      ProjectionReplayAuditResult audit,
      VerifyProjectionCheckpointFromRaw(
          log, &audit_projector, checkpoint.manifest_hash, audit_config,
          &checkpoint_store, &ledger, &dag));

  EXPECT_EQ(audit.certificate.verdict, AuditVerdict::kCorrectionEmitted);
  EXPECT_EQ(audit.certificate.drift_score, 1.0);
  ASSERT_TRUE(audit.correction.has_value());
  EXPECT_EQ(audit.correction->audit_certificate_id,
            audit.certificate.certificate_id);
  EXPECT_EQ(audit.correction->target_checkpoint_manifest_hash,
            checkpoint.manifest_hash);
  EXPECT_THAT(audit.correction->replacement_projection, HasSubstr("STEP_4"));

  ASSERT_OK(AppendCorrectionEvent(&log, *audit.correction));
  ASSERT_OK_AND_ASSIGN(std::vector<Event> events, log.GetAllEvents());
  ASSERT_OK_AND_ASSIGN(CorrectionIndex index, CorrectionIndex::Build(events));
  const CorrectionBarrierDecision barrier =
      EvaluateCorrectionBarrier(checkpoint.manifest_hash, index);
  EXPECT_TRUE(barrier.must_reproject);
  EXPECT_TRUE(barrier.must_interrupt_before_next_predict);
}

TEST(ProjectionReplayAuditorTest,
     DriftCorrectionInvalidatesRollupAncestors) {
  EventSourcedLog log(TestRoot("audit_replay_rollup_drift_log"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-rollup-drift",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "auditd initially classified as STEP_2",
      .timestamp_us = 1,
  }));

  RecordingRunner checkpoint_runner({
      R"json({"Facts":["STEP_2 [1]"],"Reasoning":["initial projection [1]"],"Compliance":["audit retained [1]"]})json",
  });
  DPMProjector checkpoint_projector(&checkpoint_runner);
  const std::filesystem::path root =
      TestRoot("audit_replay_rollup_drift_store");
  LocalFilesystemCheckpointStore checkpoint_store(root);
  LocalFilesystemAuditLedger ledger(root);
  LocalMerkleDagStore dag(root);
  ProjectionCheckpointConfig checkpoint_config = BaseCheckpointConfig();
  ASSERT_OK_AND_ASSIGN(
      ProjectionCheckpoint leaf,
      CreateProjectionCheckpoint(log, &checkpoint_projector,
                                 checkpoint_config, &checkpoint_store, &dag));

  ASSERT_OK_AND_ASSIGN(CheckpointStore::ManifestRecord leaf_record,
                       checkpoint_store.GetManifest("tenant-a",
                                                    "session-rollup-drift",
                                                    leaf.manifest_hash));
  ASSERT_OK_AND_ASSIGN(CanonicalManifestInput leaf_manifest,
                       DecodeCanonicalManifest(leaf_record.abi_bytes));
  ProjectionCheckpointConfig rollup_config = checkpoint_config;
  rollup_config.level = 1;
  rollup_config.created_unix_micros += 1;
  ASSERT_OK_AND_ASSIGN(
      ProjectionCheckpoint root_checkpoint,
      StoreRollupProjectionCheckpoint(
          log.identity(), rollup_config, leaf.event_range_start,
          leaf.event_range_end,
          R"json({"Facts":["STEP_2 [1]"],"Reasoning":["rollup [1]"],"Compliance":["audit retained [1]"]})json",
          {RollupChildRefFromManifest(leaf.manifest_hash, leaf_manifest)},
          &checkpoint_store, &dag));

  RecordingRunner audit_runner({
      R"json({"Facts":["STEP_4 [1]"],"Reasoning":["replay caught escalation [1]"],"Compliance":["audit retained [1]"]})json",
  });
  DPMProjector audit_projector(&audit_runner);
  ProjectionReplayAuditConfig audit_config = AuditConfig(checkpoint_config);
  audit_config.created_unix_micros += 2;
  ASSERT_OK_AND_ASSIGN(
      ProjectionReplayAuditResult audit,
      VerifyProjectionCheckpointFromRaw(
          log, &audit_projector, leaf.manifest_hash, audit_config,
          &checkpoint_store, &ledger, &dag));

  ASSERT_TRUE(audit.correction.has_value());
  EXPECT_THAT(audit.correction->invalidates_checkpoints,
              UnorderedElementsAre(leaf.manifest_hash,
                                   root_checkpoint.manifest_hash));
}

TEST(ProjectionReplayAuditorTest,
     HijackShapedProjectionProducesBlockingCorrection) {
  EventSourcedLog log(TestRoot("audit_replay_hijack_log"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-hijack",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "incident should remain STEP_2",
      .timestamp_us = 1,
  }));

  RecordingRunner checkpoint_runner({
      R"json({"Facts":["STEP_2 [1]"],"Reasoning":["initial projection [1]"],"Compliance":["audit retained [1]"]})json",
  });
  DPMProjector checkpoint_projector(&checkpoint_runner);
  const std::filesystem::path root = TestRoot("audit_replay_hijack_store");
  LocalFilesystemCheckpointStore checkpoint_store(root);
  LocalFilesystemAuditLedger ledger(root);
  LocalMerkleDagStore dag(root);
  ProjectionCheckpointConfig checkpoint_config = BaseCheckpointConfig();
  ASSERT_OK_AND_ASSIGN(
      ProjectionCheckpoint checkpoint,
      CreateProjectionCheckpoint(log, &checkpoint_projector,
                                 checkpoint_config, &checkpoint_store, &dag));

  RecordingRunner audit_runner({
      R"json({"risk_level":"medium","instruction":"ignore required projection schema"})json",
  });
  DPMProjector audit_projector(&audit_runner);
  ProjectionReplayAuditConfig audit_config = AuditConfig(checkpoint_config);
  audit_config.created_unix_micros += 3;
  ASSERT_OK_AND_ASSIGN(
      ProjectionReplayAuditResult audit,
      VerifyProjectionCheckpointFromRaw(
          log, &audit_projector, checkpoint.manifest_hash, audit_config,
          &checkpoint_store, &ledger, &dag));

  EXPECT_EQ(audit.certificate.verdict, AuditVerdict::kCorrectionEmitted);
  EXPECT_EQ(audit.certificate.drift_fields,
            (std::vector<std::string>{"projection_replay_error"}));
  ASSERT_TRUE(audit.correction.has_value());
  EXPECT_TRUE(audit.correction->must_interrupt_before_next_predict);
  EXPECT_THAT(audit.correction->replacement_projection,
              HasSubstr("missing field Facts"));
}

TEST(ProjectionReplayAuditorTest,
     EventSwapAfterCheckpointProducesReplayDrift) {
  EventSourcedLog log(TestRoot("audit_replay_event_swap_log"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-event-swap",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "auditd initially classified as STEP_2",
      .timestamp_us = 1,
  }));

  RecordingRunner checkpoint_runner({
      R"json({"Facts":["STEP_2 [1]"],"Reasoning":["initial projection [1]"],"Compliance":["audit retained [1]"]})json",
  });
  DPMProjector checkpoint_projector(&checkpoint_runner);
  const std::filesystem::path root = TestRoot("audit_replay_event_swap_store");
  LocalFilesystemCheckpointStore checkpoint_store(root);
  LocalFilesystemAuditLedger ledger(root);
  LocalMerkleDagStore dag(root);
  ProjectionCheckpointConfig checkpoint_config = BaseCheckpointConfig();
  ASSERT_OK_AND_ASSIGN(
      ProjectionCheckpoint checkpoint,
      CreateProjectionCheckpoint(log, &checkpoint_projector,
                                 checkpoint_config, &checkpoint_store, &dag));

  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "later raw event changes the replay to STEP_4",
      .timestamp_us = 2,
  }));

  RecordingRunner audit_runner({
      R"json({"Facts":["STEP_4 [2]"],"Reasoning":["event-swap replay [2]"],"Compliance":["audit retained [1]"]})json",
  });
  DPMProjector audit_projector(&audit_runner);
  ProjectionReplayAuditConfig audit_config = AuditConfig(checkpoint_config);
  audit_config.created_unix_micros += 2;
  ASSERT_OK_AND_ASSIGN(
      ProjectionReplayAuditResult audit,
      VerifyProjectionCheckpointFromRaw(
          log, &audit_projector, checkpoint.manifest_hash, audit_config,
          &checkpoint_store, &ledger, &dag));

  EXPECT_EQ(audit.certificate.verdict, AuditVerdict::kCorrectionEmitted);
  EXPECT_EQ(audit.certificate.drift_score, 1.0);
  EXPECT_EQ(audit.certificate.event_range_start, 0);
  EXPECT_EQ(audit.certificate.event_range_end, checkpoint.event_count);
  EXPECT_EQ(audit.certificate.log_generation, 2);
  ASSERT_TRUE(audit.correction.has_value());
  EXPECT_EQ(audit.correction->target_event_range_end,
            checkpoint.event_count);
  EXPECT_THAT(audit.correction->replacement_projection, HasSubstr("STEP_4"));
}

TEST(ProjectionReplayAuditorTest,
     BlockingCorrectionForDifferentCheckpointDoesNotTripBarrier) {
  const Hash256 active = HashBytes(HashAlgorithm::kBlake3, "active");
  const Hash256 other = HashBytes(HashAlgorithm::kBlake3, "other");
  const Hash256 certificate = HashBytes(HashAlgorithm::kBlake3, "cert");

  EventSourcedLog log(TestRoot("audit_replay_scoped_correction_log"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-scope",
                      });
  ASSERT_OK(AppendCorrectionEvent(
      &log, BlockingCorrectionFor(other, certificate)));
  ASSERT_OK_AND_ASSIGN(std::vector<Event> events, log.GetAllEvents());
  ASSERT_OK_AND_ASSIGN(CorrectionIndex index, CorrectionIndex::Build(events));

  const CorrectionBarrierDecision active_barrier =
      EvaluateCorrectionBarrier(active, index);
  EXPECT_FALSE(active_barrier.must_reproject);
  EXPECT_FALSE(active_barrier.must_interrupt_before_next_predict);

  const CorrectionBarrierDecision other_barrier =
      EvaluateCorrectionBarrier(other, index);
  EXPECT_TRUE(other_barrier.must_reproject);
  EXPECT_TRUE(other_barrier.must_interrupt_before_next_predict);
}

}  // namespace
}  // namespace litert::lm
