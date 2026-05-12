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

#include "runtime/dpm/active_evidence_view.h"

#include <filesystem>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "runtime/dpm/event.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/dpm/projection_prompt.h"
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

EventSourcedLog MakeLog(absl::string_view name) {
  return EventSourcedLog(TestPath(name),
                         DPMLogIdentity{
                             .tenant_id = "tenant-a",
                             .session_id = "session-1",
                         });
}

TEST(ActiveEvidenceViewTest, PreservesActiveEventsWithoutCorrections) {
  EventSourcedLog log = MakeLog("active_view_no_corrections");
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "transport as main result",
      .timestamp_us = 100,
  }));

  ASSERT_OK_AND_ASSIGN(ActiveEvidenceView view,
                       BuildActiveEvidenceView(log, 0, 1, {}));

  EXPECT_EQ(view.event_range_start, 0);
  EXPECT_EQ(view.event_range_end, 1);
  EXPECT_THAT(view.active_event_log, HasSubstr("[1] {"));
  EXPECT_THAT(view.active_event_log, HasSubstr("transport as main result"));
  EXPECT_TRUE(view.revoked_evidence_log.empty());
  EXPECT_TRUE(view.revoked_records.empty());
}

TEST(ActiveEvidenceViewTest, RevokesPriorInvalidatedFactsFromActiveView) {
  EventSourcedLog log = MakeLog("active_view_prior_revocation");
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
  const std::vector<ProjectionCorrectionDirective> directives = {
      ProjectionCorrectionDirective{
          .correction_event_id = "corr-transport",
          .correction_event_index = 1,
          .invalidated_facts = {"transport as main result"},
          .replacement_facts = {"credential theft is the main result [2]"},
          .scope = ProjectionCorrectionScope::kPriorEvents,
      }};

  ASSERT_OK_AND_ASSIGN(ActiveEvidenceView view,
                       BuildActiveEvidenceView(log, 0, 2, directives));

  EXPECT_THAT(view.active_event_log, HasSubstr("REVOKED_BY_CORRECTION"));
  EXPECT_THAT(view.active_event_log, HasSubstr("corr-transport"));
  EXPECT_THAT(view.active_event_log,
              Not(HasSubstr("initial analysis says transport as main result")));
  EXPECT_THAT(view.active_event_log,
              HasSubstr("correction: credential theft is the main result"));
  ASSERT_EQ(view.revoked_records.size(), 1);
  EXPECT_EQ(view.revoked_records[0].global_event_index, 0);
  EXPECT_THAT(view.revoked_records[0].original_event_json,
              HasSubstr("initial analysis says transport as main result"));
  EXPECT_THAT(view.revoked_evidence_log,
              HasSubstr("transport as main result"));
}

TEST(ActiveEvidenceViewTest, PriorScopeDoesNotRevokeLaterMatchingFacts) {
  EventSourcedLog log = MakeLog("active_view_later_fact");
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kCorrection,
      .payload = "correction issued",
      .timestamp_us = 100,
  }));
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "later report still says transport as main result",
      .timestamp_us = 200,
  }));
  const std::vector<ProjectionCorrectionDirective> directives = {
      ProjectionCorrectionDirective{
          .correction_event_id = "corr-transport",
          .correction_event_index = 0,
          .invalidated_facts = {"transport as main result"},
          .scope = ProjectionCorrectionScope::kPriorEvents,
      }};

  ASSERT_OK_AND_ASSIGN(ActiveEvidenceView view,
                       BuildActiveEvidenceView(log, 0, 2, directives));

  EXPECT_THAT(view.active_event_log,
              HasSubstr("later report still says transport as main result"));
  EXPECT_TRUE(view.revoked_records.empty());
}

TEST(ActiveEvidenceViewTest, RejectsInvalidRanges) {
  EventSourcedLog log = MakeLog("active_view_range");
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "one event",
      .timestamp_us = 100,
  }));

  EXPECT_FALSE(BuildActiveEvidenceView(log, 1, 0, {}).ok());
  EXPECT_FALSE(BuildActiveEvidenceView(log, 0, 2, {}).ok());
}

}  // namespace
}  // namespace litert::lm
