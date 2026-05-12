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

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "absl/time/clock.h"  // from @com_google_absl
#include "absl/time/time.h"  // from @com_google_absl
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "runtime/dpm/checkpoint_decision_gate.h"
#include "runtime/dpm/correction_protocol.h"
#include "runtime/dpm/dpm_projector.h"
#include "runtime/dpm/event.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/platform/eventlog/posix_event_sink.h"
#include "runtime/platform/hash/hasher.h"
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

Event UserEvent(int index, absl::string_view payload) {
  return Event{
      .type = Event::Type::kUser,
      .payload = std::string(payload),
      .timestamp_us = 1000 + index,
  };
}

class RecordingRunner : public DPMInferenceRunner {
 public:
  explicit RecordingRunner(std::string response)
      : response_(std::move(response)) {}

  absl::StatusOr<std::string> Generate(
      absl::string_view prompt, const DPMInferenceConfig& config) override {
    prompts.push_back(std::string(prompt));
    configs.push_back(config);
    return response_;
  }

  std::vector<std::string> prompts;
  std::vector<DPMInferenceConfig> configs;

 private:
  std::string response_;
};

DPMProjector::ProjectionConfig ProjectionConfig() {
  DPMProjector::ProjectionConfig config;
  config.schema_id = "incident_response_v1";
  config.schema_json =
      R"json({"Facts":["string with [i]"],"Reasoning":["string with [i]"],"Compliance":["string with [i]"]})json";
  config.model_id = "perf-smoke-model";
  config.memory_budget_chars = 2048;
  return config;
}

void ExpectUnder(absl::Duration elapsed, absl::Duration limit,
                 absl::string_view label) {
  EXPECT_LT(elapsed, limit) << label << " took " << elapsed
                            << " which exceeds local smoke limit " << limit;
}

TEST(DpmPerfSmokeTest, BranchRangeReadStaysBelowLocalThreshold) {
  const std::filesystem::path root = TestRoot("dpm_perf_branch_range");
  PosixEventSink sink(root);
  EventSourcedLog parent(&sink, DPMLogIdentity{
                                    .tenant_id = "tenant-a",
                                    .session_id = "parent",
                                });
  for (int i = 0; i < 64; ++i) {
    ASSERT_OK(parent.Append(UserEvent(i, "parent event")));
  }
  ASSERT_OK(sink.CreateBranch("tenant-a", "parent", "tenant-a", "branch",
                              32));
  EventSourcedLog branch(&sink, DPMLogIdentity{
                                    .tenant_id = "tenant-a",
                                    .session_id = "branch",
                                });
  for (int i = 0; i < 8; ++i) {
    ASSERT_OK(branch.Append(UserEvent(i, "branch event")));
  }

  int seen = 0;
  const absl::Time start = absl::Now();
  ASSERT_OK(sink.ForEachRecordRange(
      "tenant-a", "branch", 30, 36,
      [&](absl::string_view record) -> absl::Status {
        ++seen;
        return absl::OkStatus();
      }));
  const absl::Duration elapsed = absl::Now() - start;

  EXPECT_EQ(seen, 6);
  ExpectUnder(elapsed, absl::Seconds(1), "branch range read");
}

TEST(DpmPerfSmokeTest, CorrectionIndexLookupSurvivesCorruptedLogTail) {
  EventSourcedLog log(TestRoot("dpm_perf_correction_index"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-index",
                      });
  for (int i = 0; i < 64; ++i) {
    ASSERT_OK(log.Append(UserEvent(i, "filler event")));
  }
  const Hash256 checkpoint = HashBytes(HashAlgorithm::kBlake3, "checkpoint");
  CorrectionPayload payload;
  payload.correction_id = "corr-perf-smoke";
  payload.target_checkpoint_manifest_hash = checkpoint;
  payload.target_event_range_start = 0;
  payload.target_event_range_end = 64;
  payload.audit_certificate_id =
      HashBytes(HashAlgorithm::kBlake3, "certificate");
  payload.reason_code = "projection_replay_mismatch";
  payload.severity = CorrectionSeverity::kBlocking;
  payload.invalidates_checkpoints = {checkpoint};
  payload.must_interrupt_before_next_predict = true;
  payload.created_unix_micros = 1777390000000000;
  ASSERT_OK(AppendCorrectionEvent(&log, payload));

  std::ofstream out(log.path(), std::ios::out | std::ios::app |
                                    std::ios::binary);
  ASSERT_TRUE(out.is_open());
  out << "not-a-valid-framed-record";
  out.close();

  const absl::Time start = absl::Now();
  ASSERT_OK_AND_ASSIGN(CorrectionIndex index,
                       CorrectionIndex::LoadForCheckpoint(log, checkpoint));
  const absl::Duration elapsed = absl::Now() - start;

  const CorrectionBarrierDecision barrier =
      EvaluateCorrectionBarrier(checkpoint, index);
  EXPECT_TRUE(barrier.must_reproject);
  EXPECT_TRUE(barrier.must_interrupt_before_next_predict);
  ExpectUnder(elapsed, absl::Seconds(1), "correction sidecar lookup");
}

TEST(DpmPerfSmokeTest, CorrectionReplayProjectsActiveEvidenceRangeOnce) {
  EventSourcedLog log(TestRoot("dpm_perf_correction_replay"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-replay",
                      });
  for (int i = 0; i < 64; ++i) {
    ASSERT_OK(log.Append(UserEvent(i, i == 32 ? "stale credential is valid"
                                             : "ordinary event")));
  }
  ProjectionCorrectionDirective directive;
  directive.correction_event_id = "corr-32";
  directive.correction_event_index = 100;
  directive.scope = ProjectionCorrectionScope::kPriorEvents;
  directive.invalidated_facts = {"stale credential"};
  directive.replacement_facts = {"credential was revoked [101]"};
  directive.correction_text = "stale credential was revoked";

  RecordingRunner runner(
      R"json({"Facts":["credential was revoked [101]"],"Reasoning":["blocking correction applied [101]"],"Compliance":["revoked evidence suppressed [33]"]})json");
  DPMProjector projector(&runner);

  const absl::Time start = absl::Now();
  ASSERT_OK_AND_ASSIGN(
      std::string projection,
      projector.ProjectRangeWithCorrections(log, 0, 64, ProjectionConfig(),
                                            {directive}));
  const absl::Duration elapsed = absl::Now() - start;

  EXPECT_THAT(projection, HasSubstr("credential was revoked"));
  ASSERT_EQ(runner.prompts.size(), 1);
  EXPECT_THAT(runner.prompts[0], HasSubstr("REVOKED_BY_CORRECTION"));
  ExpectUnder(elapsed, absl::Seconds(1), "correction replay projection");
}

}  // namespace
}  // namespace litert::lm
