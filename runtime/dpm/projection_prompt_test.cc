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
  ASSERT_OK_AND_ASSIGN(
      ProjectionPromptParts p1,
      CreateProjectionPromptParts("evt-A", "schema-A", kSchemaJson, 1338,
                                  1 << 20));
  ASSERT_OK_AND_ASSIGN(
      ProjectionPromptParts p2,
      CreateProjectionPromptParts("evt-B-different", "schema-A", kSchemaJson,
                                  1338, 1 << 20));
  // Same config, different event logs -> identical cacheable_prefix.
  EXPECT_EQ(p1.cacheable_prefix, p2.cacheable_prefix);
  EXPECT_THAT(p1.cacheable_prefix,
              HasSubstr("[DPM PROJECTION PREFIX BOUNDARY v1]"));
  // Suffixes differ.
  EXPECT_NE(p1.event_log_suffix, p2.event_log_suffix);
}

TEST(ProjectionPromptTest, PrefixDiffersOnAnyConfigChange) {
  ASSERT_OK_AND_ASSIGN(
      ProjectionPromptParts baseline,
      CreateProjectionPromptParts("evt", "schema-A", kSchemaJson, 1338,
                                  1 << 20));
  ASSERT_OK_AND_ASSIGN(
      ProjectionPromptParts diff_schema,
      CreateProjectionPromptParts("evt", "schema-B", kSchemaJson, 1338,
                                  1 << 20));
  ASSERT_OK_AND_ASSIGN(
      ProjectionPromptParts diff_json,
      CreateProjectionPromptParts("evt", "schema-A", R"json({"X":[]})json",
                                  1338, 1 << 20));
  ASSERT_OK_AND_ASSIGN(
      ProjectionPromptParts diff_budget,
      CreateProjectionPromptParts("evt", "schema-A", kSchemaJson, 5352,
                                  1 << 20));
  EXPECT_NE(baseline.cacheable_prefix, diff_schema.cacheable_prefix);
  EXPECT_NE(baseline.cacheable_prefix, diff_json.cacheable_prefix);
  EXPECT_NE(baseline.cacheable_prefix, diff_budget.cacheable_prefix);
}

TEST(ProjectionPromptTest, ComposeEqualsLegacyFullPrompt) {
  ASSERT_OK_AND_ASSIGN(
      ProjectionPromptParts parts,
      CreateProjectionPromptParts("event-log-bytes", "schema-A", kSchemaJson,
                                  1338, 1 << 20));
  ASSERT_OK_AND_ASSIGN(std::string full,
                       CreateProjectionPrompt("event-log-bytes", "schema-A",
                                              kSchemaJson, 1338, 1 << 20));
  EXPECT_EQ(parts.Compose(), full);
  EXPECT_THAT(full, StartsWith(parts.cacheable_prefix));
}

TEST(ProjectionPromptTest, RejectsEmptySchemaAndZeroBudget) {
  EXPECT_FALSE(
      CreateProjectionPromptParts("evt", "", kSchemaJson, 1338, 1 << 20).ok());
  EXPECT_FALSE(
      CreateProjectionPromptParts("evt", "schema-A", "", 1338, 1 << 20).ok());
  EXPECT_FALSE(
      CreateProjectionPromptParts("evt", "schema-A", kSchemaJson, 0, 1 << 20)
          .ok());
}

TEST(ProjectionPromptTest, RejectsOversizedEventLog) {
  EXPECT_FALSE(CreateProjectionPromptParts("toolarge", "schema-A", kSchemaJson,
                                           1338, /*max=*/4)
                   .ok());
}

}  // namespace
}  // namespace litert::lm
