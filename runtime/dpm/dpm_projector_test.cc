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

#include "runtime/dpm/dpm_projector.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "runtime/dpm/event.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/dpm/stateless_decision_engine.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

std::filesystem::path TestPath(absl::string_view name) {
  std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / std::string(name);
  std::filesystem::remove_all(path);
  return path;
}

class RecordingRunner : public DPMInferenceRunner {
 public:
  explicit RecordingRunner(std::string response)
      : responses_({std::move(response)}) {}
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
  size_t next_ = 0;
};

TEST(DPMProjectorTest, CreatesSchemaAnchoredDeterministicProjectionPrompt) {
  EventSourcedLog log(TestPath("dpm_projector_prompt_test"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .payload = "customer requests approval",
      .timestamp_us = 123,
  }));
  RecordingRunner runner(
      R"json({"Facts":["customer requests approval [1]"],"Reasoning":["request is explicit [1]"],"Compliance":["source cited [1]"]})json");
  DPMProjector projector(&runner);
  DPMProjector::ProjectionConfig config;
  config.max_tokens = 64;
  config.memory_budget_chars = 1338;
  config.schema_id = "insurance_liability_v2";
  config.schema_json =
      R"json({"Facts":["string with [i]"],"Reasoning":["string with [i]"],"Compliance":["string with [i]"]})json";
  config.model_id = "pinned-test-model";

  ASSERT_OK_AND_ASSIGN(std::string projection, projector.Project(log, config));

  EXPECT_THAT(projection, HasSubstr("customer requests approval [1]"));
  ASSERT_EQ(runner.prompts.size(), 1);
  EXPECT_THAT(runner.prompts[0],
              HasSubstr("decision-ready memory view over an event log"));
  EXPECT_THAT(runner.prompts[0], HasSubstr("insurance_liability_v2"));
  EXPECT_THAT(runner.prompts[0], HasSubstr("[1] {"));
  EXPECT_THAT(runner.prompts[0], HasSubstr("[MEMORY BUDGET]\n1338"));
  EXPECT_THAT(runner.prompts[0], Not(HasSubstr("\"index\"")));
  EXPECT_THAT(runner.prompts[0], HasSubstr("customer requests approval"));
  ASSERT_EQ(runner.configs.size(), 1);
  EXPECT_EQ(runner.configs[0].max_output_tokens, 64);
  EXPECT_EQ(runner.configs[0].seed, 42);
  EXPECT_EQ(runner.configs[0].temperature, 0.0f);
  EXPECT_TRUE(runner.configs[0].fresh_context);
  EXPECT_EQ(runner.configs[0].model_id, "pinned-test-model");
}

TEST(DPMProjectorTest, ReplaysSameLogToByteIdenticalPrompt) {
  std::string first_prompt;
  for (int i = 0; i < 10; ++i) {
    EventSourcedLog log(TestPath(absl::StrCat("dpm_projector_determinism_", i)),
                        DPMLogIdentity{
                            .tenant_id = "tenant-a",
                            .session_id = "session-1",
                        });
    ASSERT_OK(log.Append(Event{
        .type = Event::Type::kUser,
        .payload = "fact A",
        .timestamp_us = 100,
    }));
    ASSERT_OK(log.Append(Event{
        .type = Event::Type::kCorrection,
        .payload = "fact A corrected",
        .timestamp_us = 200,
    }));
    DPMProjector projector(nullptr);
    DPMProjector::ProjectionConfig config;
    config.schema_id = "insurance_liability_v2";
    config.schema_json =
        R"json({"Facts":["string with [i]"],"Reasoning":["string with [i]"],"Compliance":["string with [i]"]})json";
    config.model_id = "pinned-test-model";

    ASSERT_OK_AND_ASSIGN(
        std::string prompt,
        projector.CreateProjectionPrompt(log, config));
    if (i == 0) {
      first_prompt = prompt;
    }
    EXPECT_EQ(prompt, first_prompt);
  }
}

TEST(DPMProjectorTest, RejectsMissingSchemaAndInvalidProjectionJson) {
  EventSourcedLog log(TestPath("dpm_projector_invalid_test"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .payload = "fact A",
      .timestamp_us = 100,
  }));
  RecordingRunner runner("not json");
  DPMProjector projector(&runner);
  DPMProjector::ProjectionConfig config;
  config.model_id = "pinned-test-model";

  EXPECT_FALSE(projector.CreateProjectionPrompt(log, config).ok());

  config.schema_id = "insurance_liability_v2";
  config.schema_json =
      R"json({"Facts":["string with [i]"],"Reasoning":["string with [i]"],"Compliance":["string with [i]"]})json";
  EXPECT_FALSE(projector.Project(log, config).ok());
}

TEST(DPMProjectorTest, RejectsProjectionWithoutOneBasedCitations) {
  EventSourcedLog log(TestPath("dpm_projector_citation_test"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .payload = "fact A",
      .timestamp_us = 100,
  }));
  RecordingRunner runner(
      R"json({"Facts":["fact A [0]"],"Reasoning":["because [1]"],"Compliance":["ok [1]"]})json");
  DPMProjector projector(&runner);
  DPMProjector::ProjectionConfig config;
  config.schema_id = "insurance_liability_v2";
  config.schema_json =
      R"json({"Facts":["string with [i]"],"Reasoning":["string with [i]"],"Compliance":["string with [i]"]})json";
  config.model_id = "pinned-test-model";

  EXPECT_FALSE(projector.Project(log, config).ok());
}

TEST(DPMProjectorTest,
     ProjectRangeWithCorrectionsInjectsBlockingDirectiveAndPassesCleanOutput) {
  EventSourcedLog log(TestPath("dpm_projector_correction_prompt_test"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "initial analysis says transport as main result",
      .timestamp_us = 100,
  }));
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kCorrection,
      .payload = "correction: credential theft is the main result",
      .timestamp_us = 200,
  }));
  RecordingRunner runner(
      R"json({"Facts":["credential theft is the main result [2]"],"Reasoning":["correction supersedes earlier analysis [2]"],"Compliance":["audit trail retained [2]"]})json");
  DPMProjector projector(&runner);
  DPMProjector::ProjectionConfig config;
  config.schema_id = "incident_response_v1";
  config.schema_json =
      R"json({"Facts":["string with [i]"],"Reasoning":["string with [i]"],"Compliance":["string with [i]"]})json";
  config.model_id = "pinned-test-model";
  const std::vector<ProjectionCorrectionDirective> directives = {
      ProjectionCorrectionDirective{
          .correction_event_id = "corr-transport",
          .correction_event_index = 1,
          .correction_text = "Transport was not the main result.",
          .invalidated_facts = {"transport as main result"},
          .replacement_facts = {"credential theft is the main result [2]"},
      }};

  ASSERT_OK_AND_ASSIGN(
      std::string projection,
      projector.ProjectRangeWithCorrections(log, 0, 2, config, directives));

  EXPECT_THAT(projection, HasSubstr("credential theft is the main result"));
  EXPECT_THAT(projection, Not(HasSubstr("transport as main result")));
  ASSERT_EQ(runner.prompts.size(), 1);
  EXPECT_THAT(runner.prompts[0], HasSubstr("[BLOCKING CORRECTIONS]"));
  EXPECT_THAT(runner.prompts[0], HasSubstr("corr-transport"));
  EXPECT_THAT(runner.prompts[0], HasSubstr("transport as main result"));
}

TEST(DPMProjectorTest, CorrectionAwareReplayMarksRevokedEvidenceBeforePrompt) {
  EventSourcedLog log(TestPath("dpm_projector_correction_view_test"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "initial analysis says transport as main result",
      .timestamp_us = 100,
  }));
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kCorrection,
      .payload = "correction: credential theft is the main result",
      .timestamp_us = 200,
  }));
  RecordingRunner runner(
      R"json({"Facts":["credential theft is the main result [2]"],"Reasoning":["correction supersedes earlier analysis [2]"],"Compliance":["audit trail retained [2]"]})json");
  DPMProjector projector(&runner);
  DPMProjector::ProjectionConfig config;
  config.schema_id = "incident_response_v1";
  config.schema_json =
      R"json({"Facts":["string with [i]"],"Reasoning":["string with [i]"],"Compliance":["string with [i]"]})json";
  config.model_id = "pinned-test-model";
  const std::vector<ProjectionCorrectionDirective> directives = {
      ProjectionCorrectionDirective{
          .correction_event_id = "corr-transport",
          .correction_event_index = 1,
          .correction_text = "Transport was not the main result.",
          .invalidated_facts = {"transport as main result"},
          .replacement_facts = {"credential theft is the main result [2]"},
          .scope = ProjectionCorrectionScope::kPriorEvents,
      }};

  ASSERT_OK_AND_ASSIGN(
      std::string prompt,
      projector.CreateProjectionPromptForRangeWithCorrections(
          log, 0, 2, config, directives));

  EXPECT_THAT(prompt, HasSubstr("[BLOCKING CORRECTIONS]"));
  EXPECT_THAT(prompt, HasSubstr("invalidated_facts"));
  EXPECT_THAT(prompt, HasSubstr("transport as main result"));
  EXPECT_THAT(prompt, HasSubstr("REVOKED_BY_CORRECTION"));
  EXPECT_THAT(prompt, HasSubstr("corr-transport"));
  EXPECT_THAT(prompt, Not(HasSubstr(
                          "initial analysis says transport as main result")));
  EXPECT_THAT(prompt,
              HasSubstr("correction: credential theft is the main result"));
}

TEST(DPMProjectorTest, ProjectRangeWithCorrectionsRepairsLeakedOldFact) {
  EventSourcedLog log(TestPath("dpm_projector_correction_repair_test"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "initial analysis says transport as main result",
      .timestamp_us = 100,
  }));
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kCorrection,
      .payload = "correction: credential theft is the main result",
      .timestamp_us = 200,
  }));
  RecordingRunner runner(std::vector<std::string>{
      R"json({"Facts":["transport as main result [1]"],"Reasoning":["old path [1]"],"Compliance":["audit trail retained [1]"]})json",
      R"json({"Facts":["credential theft is the main result [2]"],"Reasoning":["correction supersedes earlier analysis [2]"],"Compliance":["audit trail retained [2]"]})json",
  });
  DPMProjector projector(&runner);
  DPMProjector::ProjectionConfig config;
  config.schema_id = "incident_response_v1";
  config.schema_json =
      R"json({"Facts":["string with [i]"],"Reasoning":["string with [i]"],"Compliance":["string with [i]"]})json";
  config.model_id = "pinned-test-model";
  const std::vector<ProjectionCorrectionDirective> directives = {
      ProjectionCorrectionDirective{
          .correction_event_id = "corr-transport",
          .correction_event_index = 1,
          .invalidated_facts = {"transport as main result"},
          .replacement_facts = {"credential theft is the main result [2]"},
      }};

  ASSERT_OK_AND_ASSIGN(
      std::string projection,
      projector.ProjectRangeWithCorrections(log, 0, 2, config, directives));

  EXPECT_THAT(projection, HasSubstr("credential theft is the main result"));
  EXPECT_THAT(projection, Not(HasSubstr("transport as main result")));
  ASSERT_EQ(runner.prompts.size(), 2);
  EXPECT_THAT(runner.prompts[1], HasSubstr("CORRECTION REPAIR"));
  EXPECT_THAT(runner.prompts[1], HasSubstr("[FORBIDDEN SUBSTRINGS]"));
}

TEST(DPMProjectorTest,
     ProjectRangeWithCorrectionsFailsClosedWhenRepairStillLeaks) {
  EventSourcedLog log(TestPath("dpm_projector_correction_fail_closed_test"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "initial analysis says transport as main result",
      .timestamp_us = 100,
  }));
  RecordingRunner runner(std::vector<std::string>{
      R"json({"Facts":["transport as main result [1]"],"Reasoning":["old path [1]"],"Compliance":["audit trail retained [1]"]})json",
      R"json({"Facts":["transport as main result [1]"],"Reasoning":["still old [1]"],"Compliance":["audit trail retained [1]"]})json",
  });
  DPMProjector projector(&runner);
  DPMProjector::ProjectionConfig config;
  config.schema_id = "incident_response_v1";
  config.schema_json =
      R"json({"Facts":["string with [i]"],"Reasoning":["string with [i]"],"Compliance":["string with [i]"]})json";
  config.model_id = "pinned-test-model";
  const std::vector<ProjectionCorrectionDirective> directives = {
      ProjectionCorrectionDirective{
          .correction_event_id = "corr-transport",
          .invalidated_facts = {"transport as main result"},
      }};

  absl::StatusOr<std::string> projection =
      projector.ProjectRangeWithCorrections(log, 0, 1, config, directives);

  ASSERT_FALSE(projection.ok());
  EXPECT_EQ(projection.status().code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_THAT(std::string(projection.status().message()),
              HasSubstr("invalidated fact"));
}

TEST(DPMProjectorTest, UnrelatedCorrectionDoesNotSuppressValidFact) {
  EventSourcedLog log(TestPath("dpm_projector_unrelated_correction_test"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "initial analysis says transport as main result",
      .timestamp_us = 100,
  }));
  RecordingRunner runner(
      R"json({"Facts":["transport as main result [1]"],"Reasoning":["signal retained [1]"],"Compliance":["audit trail retained [1]"]})json");
  DPMProjector projector(&runner);
  DPMProjector::ProjectionConfig config;
  config.schema_id = "incident_response_v1";
  config.schema_json =
      R"json({"Facts":["string with [i]"],"Reasoning":["string with [i]"],"Compliance":["string with [i]"]})json";
  config.model_id = "pinned-test-model";
  const std::vector<ProjectionCorrectionDirective> directives = {
      ProjectionCorrectionDirective{
          .correction_event_id = "corr-unrelated",
          .invalidated_facts = {"database state changed"},
      }};

  ASSERT_OK_AND_ASSIGN(
      std::string projection,
      projector.ProjectRangeWithCorrections(log, 0, 1, config, directives));

  EXPECT_THAT(projection, HasSubstr("transport as main result"));
  ASSERT_EQ(runner.prompts.size(), 1);
}

TEST(StatelessDecisionEngineTest, AppendsRequestProjectsThenAppendsDecision) {
  EventSourcedLog log(TestPath("dpm_stateless_decision_test"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  RecordingRunner projection_runner(
      R"json({"Facts":["ok [1]"],"Reasoning":["request supports decision [1]"],"Compliance":["citation present [1]"]})json");
  RecordingRunner decision_runner("Approve");
  DPMProjector projector(&projection_runner);
  StatelessDecisionEngine engine(&log, &projector, &decision_runner);
  StatelessDecisionEngine::Config config;
  config.projection.schema_id = "insurance_liability_v2";
  config.projection.schema_json =
      R"json({"Facts":["string with [i]"],"Reasoning":["string with [i]"],"Compliance":["string with [i]"]})json";
  config.model_id = "pinned-test-model";

  ASSERT_OK_AND_ASSIGN(
      DPMDecisionResponse response,
      engine.Decide(DPMDecisionRequest{
                        .payload = "case input",
                        .case_id = "CASE-7",
                        .timestamp_us = 111,
                        .response_timestamp_us = 222,
                    },
                    config));

  EXPECT_THAT(response.projected_memory,
              HasSubstr(R"json("Facts":["ok [1]"])json"));
  EXPECT_THAT(response.decision_text, HasSubstr("Approve"));
  ASSERT_EQ(projection_runner.configs.size(), 1);
  EXPECT_TRUE(projection_runner.configs[0].fresh_context);
  EXPECT_EQ(projection_runner.configs[0].temperature, 0.0f);
  ASSERT_EQ(decision_runner.configs.size(), 1);
  EXPECT_TRUE(decision_runner.configs[0].fresh_context);
  EXPECT_EQ(decision_runner.configs[0].seed, 42);
  EXPECT_THAT(decision_runner.prompts[0], HasSubstr("CASE-7"));
  EXPECT_THAT(decision_runner.prompts[0], HasSubstr("[PROJECTED MEMORY]"));

  ASSERT_OK_AND_ASSIGN(std::vector<Event> events, log.GetAllEvents());
  ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(events[0].type, Event::Type::kUser);
  EXPECT_EQ(events[0].payload, "case input");
  EXPECT_EQ(events[0].timestamp_us, 111);
  EXPECT_EQ(events[1].type, Event::Type::kModel);
  EXPECT_THAT(events[1].payload, HasSubstr("Approve"));
  EXPECT_EQ(events[1].timestamp_us, 222);
  EXPECT_EQ(events[1].model_id, "pinned-test-model");
}

TEST(StatelessDecisionEngineTest, RejectsMissingReplayTimestampsByDefault) {
  EventSourcedLog log(TestPath("dpm_stateless_timestamp_test"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  RecordingRunner projection_runner(
      R"json({"Facts":["ok [1]"],"Reasoning":["request supports decision [1]"],"Compliance":["citation present [1]"]})json");
  RecordingRunner decision_runner("Approve");
  DPMProjector projector(&projection_runner);
  StatelessDecisionEngine engine(&log, &projector, &decision_runner);
  StatelessDecisionEngine::Config config;
  config.projection.schema_id = "insurance_liability_v2";
  config.projection.schema_json =
      R"json({"Facts":["string with [i]"],"Reasoning":["string with [i]"],"Compliance":["string with [i]"]})json";
  config.model_id = "pinned-test-model";

  EXPECT_FALSE(engine.Decide(DPMDecisionRequest{
                                 .payload = "case input",
                                 .case_id = "CASE-7",
                             },
                             config)
                   .ok());
}

}  // namespace
}  // namespace litert::lm
