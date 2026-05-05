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

#include "runtime/dpm/projection_comparator.h"

#include <string>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

TEST(ProjectionComparatorTest, MatchingStructuredProjectionHasZeroDrift) {
  const std::string projection =
      R"json({"Facts":["T1021 [1]"],"Reasoning":["stage [1]"],"Compliance":["retained [1]"]})json";

  ASSERT_OK_AND_ASSIGN(ProjectionComparisonResult result,
                       CompareStructuredProjectionJson(projection, projection));

  EXPECT_TRUE(result.matches);
  EXPECT_EQ(result.drift_score, 0.0);
  EXPECT_TRUE(result.drift_fields.empty());
}

TEST(ProjectionComparatorTest, FieldDriftProducesDeterministicScore) {
  const std::string expected =
      R"json({"Facts":["T1021 [1]"],"Reasoning":["stage [1]"],"Compliance":["retained [1]"]})json";
  const std::string observed =
      R"json({"Facts":["T1021 [1]"],"Reasoning":["different [1]"],"Compliance":["retained [1]"]})json";

  ASSERT_OK_AND_ASSIGN(ProjectionComparisonResult result,
                       CompareStructuredProjectionJson(expected, observed));

  EXPECT_FALSE(result.matches);
  EXPECT_EQ(result.drift_score, 1.0 / 3.0);
  EXPECT_THAT(result.drift_fields, ElementsAre("Reasoning"));
}

TEST(ProjectionComparatorTest, MissingAndParseFailuresAreExplicit) {
  const std::string expected =
      R"json({"Facts":["T1021 [1]"],"Reasoning":["stage [1]"],"Compliance":["retained [1]"]})json";
  const std::string missing =
      R"json({"Facts":["T1021 [1]"],"Compliance":["retained [1]"]})json";

  ASSERT_OK_AND_ASSIGN(ProjectionComparisonResult field_result,
                       CompareStructuredProjectionJson(expected, missing));
  EXPECT_EQ(field_result.drift_score, 1.0 / 3.0);
  EXPECT_THAT(field_result.drift_fields, ElementsAre("Reasoning"));

  ASSERT_OK_AND_ASSIGN(ProjectionComparisonResult parse_result,
                       CompareStructuredProjectionJson(expected, "{not json"));
  EXPECT_FALSE(parse_result.matches);
  EXPECT_EQ(parse_result.drift_score, 1.0);
  ASSERT_EQ(parse_result.drift_fields.size(), 1);
  EXPECT_THAT(parse_result.drift_fields[0], HasSubstr("parse_error"));
}

}  // namespace
}  // namespace litert::lm
