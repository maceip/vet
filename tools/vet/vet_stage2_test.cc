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

#include "tools/vet/vet_aid.h"
#include "tools/vet/vet_trace.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "gtest/gtest.h"
#include "runtime/dpm/event.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/dpm/projection_prompt.h"
#include "runtime/util/test_utils.h"

namespace litert::lm::vet {
namespace {

std::filesystem::path TestRoot(absl::string_view name) {
  std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / std::string(name);
  std::filesystem::remove_all(path);
  return path;
}

TEST(VetAidTest, WritesAndReadsDefaultAid) {
  const SessionPaths paths =
      MakeSessionPaths(TestRoot("vet_aid_test").string(), "tenant", "session");
  const nlohmann::ordered_json aid = BuildDefaultAid(paths, "claude-test", 42);
  ASSERT_OK(WriteAidFile(paths, aid));
  ASSERT_TRUE(std::filesystem::exists(paths.aid_path));

  ASSERT_OK_AND_ASSIGN(nlohmann::ordered_json loaded, ReadAidFile(paths));
  EXPECT_EQ(loaded["tenant_id"], "tenant");
  EXPECT_EQ(loaded["session_id"], "session");
  EXPECT_EQ(loaded["aid_version"], kAidVersion);
  ASSERT_TRUE(loaded.contains("claims"));
  ASSERT_TRUE(loaded["claims"].contains("does_not_verify"));
  const auto& components = loaded["components"];
  ASSERT_TRUE(components.is_array());
  ASSERT_EQ(components.size(), 2);
  EXPECT_EQ(components[0]["id"], "event_log");
  EXPECT_EQ(components[1]["id"], "projection");
}

TEST(VetTraceTest, HandoffBundleVerifiesAgainstLiveLog) {
  const std::filesystem::path root = TestRoot("vet_trace_verify");
  const SessionPaths paths = MakeSessionPaths(root.string(), "local", "demo");

  nlohmann::ordered_json aid = BuildDefaultAid(paths, "", 100);
  ASSERT_OK(WriteAidFile(paths, aid));
  ASSERT_OK_AND_ASSIGN(aid, ReadAidFile(paths));
  ASSERT_OK_AND_ASSIGN(std::string aid_digest, ComputeAidDigest(aid));

  EventSourcedLog log(root, DPMLogIdentity{.tenant_id = "local",
                                           .session_id = "demo"});
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .tenant_id = "local",
      .session_id = "demo",
      .payload = "ship stage 2",
      .timestamp_us = 1,
      .step_index = 1,
  }));
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .tenant_id = "local",
      .session_id = "demo",
      .payload = "bazel build //tools/vet:vet",
      .timestamp_us = 2,
      .step_index = 2,
      .tool_call_id = "tool-2",
  }));

  ASSERT_OK_AND_ASSIGN(std::vector<Event> events, log.GetAllEvents());
  ASSERT_OK_AND_ASSIGN(std::string trace_digest,
                       ComputeTraceDigestFromFile(paths.log_path.string()));

  const std::string output_text =
      BuildHandoffText("verify bundle", paths, events, {}, 40);
  HandoffBundleRequest request{
      .paths = paths,
      .task = "verify bundle",
      .output_text = output_text,
      .events = &events,
      .corrections = nullptr,
      .max_events = 40,
      .created_unix_micros = 200,
      .aid = aid,
      .aid_digest = aid_digest,
      .trace_digest = trace_digest,
  };
  const nlohmann::ordered_json bundle = BuildHandoffBundle(request);

  ASSERT_OK_AND_ASSIGN(VerifyReport report, VerifyHandoffBundle(bundle, paths));
  EXPECT_TRUE(report.verified) << report.checks_failed.size();
  EXPECT_TRUE(report.trace_report.valid);
}

TEST(VetTraceTest, DetectsTamperedTraceDigest) {
  const std::filesystem::path root = TestRoot("vet_trace_tamper");
  const SessionPaths paths = MakeSessionPaths(root.string(), "local", "bad");

  ASSERT_OK(WriteAidFile(paths, BuildDefaultAid(paths, "", 1)));
  ASSERT_OK_AND_ASSIGN(nlohmann::ordered_json aid, ReadAidFile(paths));
  ASSERT_OK_AND_ASSIGN(std::string aid_digest, ComputeAidDigest(aid));

  EventSourcedLog log(root, DPMLogIdentity{.tenant_id = "local",
                                           .session_id = "bad"});
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .tenant_id = "local",
      .session_id = "bad",
      .payload = "original",
      .timestamp_us = 1,
  }));

  ASSERT_OK_AND_ASSIGN(std::vector<Event> events, log.GetAllEvents());
  HandoffBundleRequest request{
      .paths = paths,
      .task = "tamper test",
      .output_text = "x",
      .events = &events,
      .created_unix_micros = 1,
      .aid = aid,
      .aid_digest = aid_digest,
      .trace_digest = "deadbeef",
  };
  const nlohmann::ordered_json bundle = BuildHandoffBundle(request);

  {
    std::ofstream tamper(paths.log_path, std::ios::app);
    tamper << "{\"type\":\"internal\",\"tenant_id\":\"local\","
              "\"session_id\":\"bad\",\"payload\":\"host injected\","
              "\"timestamp_us\":9}\n";
  }

  ASSERT_OK_AND_ASSIGN(VerifyReport report, VerifyHandoffBundle(bundle, paths));
  EXPECT_FALSE(report.verified);
  ASSERT_FALSE(report.checks_failed.empty());
  EXPECT_EQ(report.checks_failed[0], "trace_digest_match");
  ASSERT_TRUE(report.failure_details.contains("trace_digest_match"));
}

TEST(VetTraceTest, VeriHandoffFlowWithCorrection) {
  const std::filesystem::path root = TestRoot("vet_verihandoff");
  const SessionPaths paths =
      MakeSessionPaths(root.string(), "local", "verihandoff");

  ASSERT_OK(WriteAidFile(paths, BuildDefaultAid(paths, "demo", 1)));
  ASSERT_OK_AND_ASSIGN(nlohmann::ordered_json aid, ReadAidFile(paths));
  ASSERT_OK_AND_ASSIGN(std::string aid_digest, ComputeAidDigest(aid));

  EventSourcedLog log(root, DPMLogIdentity{.tenant_id = "local",
                                           .session_id = "verihandoff"});
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .tenant_id = "local",
      .session_id = "verihandoff",
      .payload = "Release gate requires correction-aware handoffs.",
      .timestamp_us = 1,
      .step_index = 1,
  }));
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kCorrection,
      .tenant_id = "local",
      .session_id = "verihandoff",
      .payload =
          R"({"correction_text":"Metric definition changed.","invalidated_facts":["escape counts any stale text"],"replacement_facts":["escape counts final-answer stale text only"]})",
      .timestamp_us = 2,
      .step_index = 2,
  }));

  ASSERT_OK_AND_ASSIGN(std::vector<Event> events, log.GetAllEvents());
  ASSERT_OK_AND_ASSIGN(std::string trace_digest,
                       ComputeTraceDigestFromFile(paths.log_path.string()));

  HandoffBundleRequest request{
      .paths = paths,
      .task = "Continue release notes",
      .output_text = "handoff text",
      .events = &events,
      .max_events = 40,
      .created_unix_micros = 3,
      .aid = aid,
      .aid_digest = aid_digest,
      .trace_digest = trace_digest,
  };
  const nlohmann::ordered_json bundle = BuildHandoffBundle(request);
  EXPECT_EQ(bundle["corrections"].size(), 0);

  ASSERT_OK_AND_ASSIGN(VerifyReport report, VerifyHandoffBundle(bundle, paths));
  EXPECT_FALSE(report.verified);
  EXPECT_NE(std::find(report.checks_failed.begin(), report.checks_failed.end(),
                      "correction_count_match"),
            report.checks_failed.end());
}

TEST(VetTraceTest, VeriHandoffBundleWithCorrectionsVerifies) {
  const std::filesystem::path root = TestRoot("vet_verihandoff_ok");
  const SessionPaths paths =
      MakeSessionPaths(root.string(), "local", "verihandoff-ok");

  ASSERT_OK(WriteAidFile(paths, BuildDefaultAid(paths, "demo", 1)));
  ASSERT_OK_AND_ASSIGN(nlohmann::ordered_json aid, ReadAidFile(paths));
  ASSERT_OK_AND_ASSIGN(std::string aid_digest, ComputeAidDigest(aid));

  EventSourcedLog log(root, DPMLogIdentity{.tenant_id = "local",
                                           .session_id = "verihandoff-ok"});
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .tenant_id = "local",
      .session_id = "verihandoff-ok",
      .payload = "Release gate requires correction-aware handoffs.",
      .timestamp_us = 1,
      .step_index = 1,
  }));
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kCorrection,
      .tenant_id = "local",
      .session_id = "verihandoff-ok",
      .payload =
          R"({"correction_text":"Metric definition changed.","invalidated_facts":["escape counts any stale text"],"replacement_facts":["escape counts final-answer stale text only"]})",
      .timestamp_us = 2,
      .step_index = 2,
  }));

  ASSERT_OK_AND_ASSIGN(std::vector<Event> events, log.GetAllEvents());
  ASSERT_OK_AND_ASSIGN(std::string trace_digest,
                       ComputeTraceDigestFromFile(paths.log_path.string()));

  std::vector<ProjectionCorrectionDirective> directives;
  ProjectionCorrectionDirective directive;
  directive.correction_event_index = 1;
  directive.correction_event_id = "event-2";
  directive.correction_text = "Metric definition changed.";
  directive.invalidated_facts = {"escape counts any stale text"};
  directive.replacement_facts = {"escape counts final-answer stale text only"};
  directives.push_back(std::move(directive));

  HandoffBundleRequest request{
      .paths = paths,
      .task = "Continue release notes",
      .output_text = "handoff text",
      .events = &events,
      .corrections = &directives,
      .max_events = 40,
      .created_unix_micros = 3,
      .aid = aid,
      .aid_digest = aid_digest,
      .trace_digest = trace_digest,
  };
  const nlohmann::ordered_json bundle = BuildHandoffBundle(request);
  ASSERT_EQ(bundle["corrections"].size(), 1);

  ASSERT_OK_AND_ASSIGN(VerifyReport report, VerifyHandoffBundle(bundle, paths));
  EXPECT_TRUE(report.verified) << report.checks_failed.size();
  EXPECT_TRUE(report.failure_details.empty());
}

TEST(VetTraceTest, DetectsDuplicateStepIndex) {
  const std::filesystem::path root = TestRoot("vet_trace_dup_step");
  const SessionPaths paths =
      MakeSessionPaths(root.string(), "local", "dup-step");

  EventSourcedLog log(root, DPMLogIdentity{.tenant_id = "local",
                                           .session_id = "dup-step"});
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .tenant_id = "local",
      .session_id = "dup-step",
      .payload = "one",
      .timestamp_us = 1,
      .step_index = 1,
  }));
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .tenant_id = "local",
      .session_id = "dup-step",
      .payload = "two",
      .timestamp_us = 2,
      .step_index = 1,
  }));

  ASSERT_OK_AND_ASSIGN(std::vector<Event> events, log.GetAllEvents());
  const ValidTraceReport trace = ValidateExecutionTrace(events, paths);
  EXPECT_FALSE(trace.valid);
  ASSERT_FALSE(trace.errors.empty());
  EXPECT_NE(trace.errors[0].find("duplicate step_index"), std::string::npos);
}

}  // namespace
}  // namespace litert::lm::vet
