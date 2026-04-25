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

#include "runtime/dpm/projection_prompt.h"

#include <cstddef>
#include <string>

#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

using ::testing::HasSubstr;
using ::testing::StartsWith;

constexpr absl::string_view kSchemaJson =
    R"json({"Facts":["string with [i]"]})json";

TEST(ProjectionPromptTest, PrefixIsByteIdenticalForMatchingConfig) {
  ASSERT_OK_AND_ASSIGN(std::string p1,
                       CreateProjectionPromptPrefix("schema-A", kSchemaJson,
                                                    1338));
  ASSERT_OK_AND_ASSIGN(std::string p2,
                       CreateProjectionPromptPrefix("schema-A", kSchemaJson,
                                                    1338));
  EXPECT_EQ(p1, p2);
  EXPECT_THAT(p1, HasSubstr("[DPM PROJECTION PREFIX BOUNDARY v1]"));
}

TEST(ProjectionPromptTest, PrefixDiffersOnAnyConfigChange) {
  ASSERT_OK_AND_ASSIGN(std::string baseline,
                       CreateProjectionPromptPrefix("schema-A", kSchemaJson,
                                                    1338));
  ASSERT_OK_AND_ASSIGN(std::string different_schema,
                       CreateProjectionPromptPrefix("schema-B", kSchemaJson,
                                                    1338));
  ASSERT_OK_AND_ASSIGN(std::string different_json,
                       CreateProjectionPromptPrefix("schema-A",
                                                    R"json({"X":[]})json",
                                                    1338));
  ASSERT_OK_AND_ASSIGN(std::string different_budget,
                       CreateProjectionPromptPrefix("schema-A", kSchemaJson,
                                                    5352));
  EXPECT_NE(baseline, different_schema);
  EXPECT_NE(baseline, different_json);
  EXPECT_NE(baseline, different_budget);
}

TEST(ProjectionPromptTest, FullPromptStartsWithPrefixBytes) {
  ASSERT_OK_AND_ASSIGN(std::string prefix,
                       CreateProjectionPromptPrefix("schema-A", kSchemaJson,
                                                    1338));
  ASSERT_OK_AND_ASSIGN(std::string prompt,
                       CreateProjectionPrompt("event-log-bytes",
                                              "schema-A", kSchemaJson, 1338,
                                              /*max_event_log_chars=*/1 << 20));
  EXPECT_THAT(prompt, StartsWith(prefix));
}

TEST(ProjectionPromptTest, DifferentEventLogsShareIdenticalPrefix) {
  // The whole point: two prompts with the same config but different event
  // logs must share the same bytes up to the prefix boundary so a
  // prefix-caching backend hits the cache on the second call.
  ASSERT_OK_AND_ASSIGN(std::string prompt1,
                       CreateProjectionPrompt("first event log",
                                              "schema-A", kSchemaJson, 1338,
                                              1 << 20));
  ASSERT_OK_AND_ASSIGN(std::string prompt2,
                       CreateProjectionPrompt("a different event log entirely",
                                              "schema-A", kSchemaJson, 1338,
                                              1 << 20));
  ASSERT_OK_AND_ASSIGN(std::string prefix,
                       CreateProjectionPromptPrefix("schema-A", kSchemaJson,
                                                    1338));
  ASSERT_GE(prompt1.size(), prefix.size());
  ASSERT_GE(prompt2.size(), prefix.size());
  EXPECT_EQ(prompt1.substr(0, prefix.size()),
            prompt2.substr(0, prefix.size()));
  EXPECT_EQ(prompt1.substr(0, prefix.size()), prefix);
}

TEST(ProjectionPromptTest, ConcatenatingPrefixAndTailEqualsFullPrompt) {
  ASSERT_OK_AND_ASSIGN(std::string prefix,
                       CreateProjectionPromptPrefix("schema-A", kSchemaJson,
                                                    1338));
  ASSERT_OK_AND_ASSIGN(std::string tail,
                       CreateProjectionPromptTail("evt", 1 << 20));
  ASSERT_OK_AND_ASSIGN(std::string full,
                       CreateProjectionPrompt("evt", "schema-A", kSchemaJson,
                                              1338, 1 << 20));
  EXPECT_EQ(absl::StrCat(prefix, tail), full);
}

TEST(ProjectionPromptTest, RejectsEmptySchemaAndZeroBudget) {
  EXPECT_FALSE(
      CreateProjectionPromptPrefix("", kSchemaJson, 1338).ok());
  EXPECT_FALSE(
      CreateProjectionPromptPrefix("schema-A", "", 1338).ok());
  EXPECT_FALSE(
      CreateProjectionPromptPrefix("schema-A", kSchemaJson, 0).ok());
}

TEST(ProjectionPromptTest, TailRejectsOversizedEventLog) {
  EXPECT_FALSE(CreateProjectionPromptTail("toolarge", /*max=*/4).ok());
}

}  // namespace
}  // namespace litert::lm
