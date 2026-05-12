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

#include "runtime/dpm/correction_protocol.h"

#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

using ::testing::ElementsAre;

CorrectionPayload BaseCorrection() {
  CorrectionPayload correction;
  correction.correction_id = "corr-1";
  correction.target_checkpoint_manifest_hash =
      HashBytes(HashAlgorithm::kBlake3, "checkpoint");
  correction.target_event_range_start = 0;
  correction.target_event_range_end = 4;
  correction.audit_certificate_id =
      HashBytes(HashAlgorithm::kBlake3, "certificate");
  correction.reason_code = "projection_replay_mismatch";
  correction.severity = CorrectionSeverity::kBlocking;
  correction.must_interrupt_before_next_predict = true;
  correction.created_unix_micros = 1777390000000999;
  return correction;
}

TEST(CorrectionProtocolTest,
     CompileProjectionCorrectionDirectivesNormalizesActionableFacts) {
  CorrectionPayload correction = BaseCorrection();
  correction.correction_text = "Transport was not the main result.";
  correction.invalidated_facts = {
      " transport as main result ",
      "Transport As Main Result",
      "",
  };
  correction.replacement_facts = {
      " credential theft is the main result [4] ",
      "CREDENTIAL THEFT IS THE MAIN RESULT [4]",
  };
  correction.scope = ProjectionCorrectionScope::kPriorEvents;

  ASSERT_OK_AND_ASSIGN(
      std::vector<ProjectionCorrectionDirective> directives,
      CompileProjectionCorrectionDirectives({correction}));

  ASSERT_EQ(directives.size(), 1);
  EXPECT_EQ(directives[0].correction_event_id, "corr-1");
  EXPECT_EQ(directives[0].correction_event_index, 3);
  EXPECT_EQ(directives[0].correction_text,
            "Transport was not the main result.");
  EXPECT_THAT(directives[0].invalidated_facts,
              ElementsAre("transport as main result"));
  EXPECT_THAT(directives[0].replacement_facts,
              ElementsAre("credential theft is the main result [4]"));
  EXPECT_EQ(directives[0].scope, ProjectionCorrectionScope::kPriorEvents);
}

TEST(CorrectionProtocolTest,
     CompileProjectionCorrectionDirectivesUsesReplacementProjectionFallback) {
  CorrectionPayload correction = BaseCorrection();
  correction.replacement_projection =
      R"json({"Facts":["credential theft is the main result [4]"]})json";

  ASSERT_OK_AND_ASSIGN(
      std::vector<ProjectionCorrectionDirective> directives,
      CompileProjectionCorrectionDirectives({correction}));

  ASSERT_EQ(directives.size(), 1);
  EXPECT_TRUE(directives[0].invalidated_facts.empty());
  EXPECT_THAT(
      directives[0].replacement_facts,
      ElementsAre(
          R"json({"Facts":["credential theft is the main result [4]"]})json"));
}

TEST(CorrectionProtocolTest,
     CompileProjectionCorrectionDirectivesSkipsNonBlockingCorrections) {
  CorrectionPayload correction = BaseCorrection();
  correction.severity = CorrectionSeverity::kWarning;
  correction.must_interrupt_before_next_predict = false;

  ASSERT_OK_AND_ASSIGN(
      std::vector<ProjectionCorrectionDirective> directives,
      CompileProjectionCorrectionDirectives({correction}));

  EXPECT_TRUE(directives.empty());
}

TEST(CorrectionProtocolTest,
     CompileProjectionCorrectionDirectivesRejectsUnactionableBlockingReplay) {
  CorrectionPayload correction = BaseCorrection();

  const absl::Status status =
      CompileProjectionCorrectionDirectives({correction}).status();

  EXPECT_EQ(status.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_THAT(status.message(), testing::HasSubstr("not machine-actionable"));
}

}  // namespace
}  // namespace litert::lm
