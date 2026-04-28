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

#include "runtime/dpm/projection_prefix_cache.h"

#include <cstdint>
#include <string>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/match.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "runtime/dpm/projection_prompt.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

using ::testing::HasSubstr;

constexpr absl::string_view kSchemaJson =
    R"json({"Facts":["string with [i]"],"Reasoning":[],"Compliance":[]})json";

Hash256 FilledHash(uint8_t value) {
  Hash256 hash;
  hash.bytes.fill(value);
  return hash;
}

ProjectionPrefixCacheIdentity Identity() {
  return ProjectionPrefixCacheIdentity{
      .tenant_id = "tenant-a",
      .session_id = "session-1",
      .branch_id = "main",
      .schema_id = "insurance_liability_v2",
      .memory_budget_chars = 1338,
      .model_id = "litert-local-model",
      .model_artifact_hash = FilledHash(0x11),
      .backend_id = "x86_64-xnnpack-fp16",
      .hash_algorithm = HashAlgorithm::kBlake3,
  };
}

absl::StatusOr<std::string> ProjectionPromptFor(absl::string_view event_log) {
  return CreateProjectionPrompt(event_log, "insurance_liability_v2",
                                kSchemaJson, 1338, 1 << 20);
}

TEST(ProjectionPrefixCacheTest, HitReturnsOnlyNewEventSuffix) {
  ASSERT_OK_AND_ASSIGN(
      std::string prompt_one,
      ProjectionPromptFor(R"json([1] {"type":"user","payload":"first"})json"));
  ASSERT_OK_AND_ASSIGN(
      std::string prompt_two,
      ProjectionPromptFor(
          R"json([1] {"type":"user","payload":"first"}
[2] {"type":"tool","payload":"second"})json"));
  ASSERT_TRUE(absl::StartsWith(prompt_two, prompt_one));

  ASSERT_OK_AND_ASSIGN(
      ProjectionPrefixCacheEntry entry,
      CreateProjectionPrefixCacheEntry(Identity(), kSchemaJson, prompt_one,
                                       /*event_count=*/1, FilledHash(0x22)));

  ProjectionPrefixCacheHit hit =
      EvaluateProjectionPrefixCacheHit(entry, Identity(), kSchemaJson,
                                       prompt_two);
  EXPECT_TRUE(hit.hit) << hit.reason;
  EXPECT_EQ(hit.suffix, prompt_two.substr(prompt_one.size()));
  EXPECT_EQ(prompt_one + hit.suffix, prompt_two);
}

TEST(ProjectionPrefixCacheTest, SchemaByteChangeMisses) {
  ASSERT_OK_AND_ASSIGN(std::string prompt,
                       ProjectionPromptFor("[1] {\"payload\":\"first\"}"));
  ASSERT_OK_AND_ASSIGN(
      ProjectionPrefixCacheEntry entry,
      CreateProjectionPrefixCacheEntry(Identity(), kSchemaJson, prompt,
                                       /*event_count=*/1, FilledHash(0x22)));

  ProjectionPrefixCacheHit hit = EvaluateProjectionPrefixCacheHit(
      entry, Identity(), R"json({"Facts":["different"]})json", prompt);
  EXPECT_FALSE(hit.hit);
  EXPECT_THAT(hit.reason, HasSubstr("schema_json"));
}

TEST(ProjectionPrefixCacheTest, BackendMismatchMisses) {
  ASSERT_OK_AND_ASSIGN(std::string prompt,
                       ProjectionPromptFor("[1] {\"payload\":\"first\"}"));
  ASSERT_OK_AND_ASSIGN(
      ProjectionPrefixCacheEntry entry,
      CreateProjectionPrefixCacheEntry(Identity(), kSchemaJson, prompt,
                                       /*event_count=*/1, FilledHash(0x22)));

  ProjectionPrefixCacheIdentity request = Identity();
  request.backend_id = "arm64-mldrift-fp16";
  ProjectionPrefixCacheHit hit =
      EvaluateProjectionPrefixCacheHit(entry, request, kSchemaJson, prompt);
  EXPECT_FALSE(hit.hit);
  EXPECT_THAT(hit.reason, HasSubstr("backend_id"));
}

TEST(ProjectionPrefixCacheTest, PrefixByteTamperMisses) {
  ASSERT_OK_AND_ASSIGN(
      std::string prompt_one,
      ProjectionPromptFor(R"json([1] {"type":"user","payload":"first"})json"));
  ASSERT_OK_AND_ASSIGN(
      ProjectionPrefixCacheEntry entry,
      CreateProjectionPrefixCacheEntry(Identity(), kSchemaJson, prompt_one,
                                       /*event_count=*/1, FilledHash(0x22)));
  std::string tampered_prompt = prompt_one;
  tampered_prompt[tampered_prompt.size() - 2] = 'X';

  ProjectionPrefixCacheHit hit = EvaluateProjectionPrefixCacheHit(
      entry, Identity(), kSchemaJson, tampered_prompt);
  EXPECT_FALSE(hit.hit);
  EXPECT_THAT(hit.reason, HasSubstr("prompt prefix bytes changed"));
}

TEST(ProjectionPrefixCacheTest, RejectsUnboundEntry) {
  ProjectionPrefixCacheIdentity bad = Identity();
  bad.model_artifact_hash = Hash256();
  auto entry = CreateProjectionPrefixCacheEntry(
      bad, kSchemaJson, "prompt", /*event_count=*/1, FilledHash(0x22));
  EXPECT_FALSE(entry.ok());
}

TEST(ProjectionPrefixCacheTest, InMemoryCacheReturnsLongestExactPrefix) {
  ASSERT_OK_AND_ASSIGN(
      std::string prompt_one,
      ProjectionPromptFor(R"json([1] {"payload":"first"})json"));
  ASSERT_OK_AND_ASSIGN(
      std::string prompt_two,
      ProjectionPromptFor(R"json([1] {"payload":"first"}
[2] {"payload":"second"})json"));
  ASSERT_OK_AND_ASSIGN(
      std::string prompt_three,
      ProjectionPromptFor(R"json([1] {"payload":"first"}
[2] {"payload":"second"}
[3] {"payload":"third"})json"));

  InMemoryProjectionPrefixCache cache;
  ASSERT_OK_AND_ASSIGN(
      ProjectionPrefixCacheEntry short_entry,
      CreateProjectionPrefixCacheEntry(Identity(), kSchemaJson, prompt_one,
                                       /*event_count=*/1, FilledHash(0x31)));
  ASSERT_OK_AND_ASSIGN(
      ProjectionPrefixCacheEntry long_entry,
      CreateProjectionPrefixCacheEntry(Identity(), kSchemaJson, prompt_two,
                                       /*event_count=*/2, FilledHash(0x32)));
  ASSERT_OK(cache.Store(short_entry));
  ASSERT_OK(cache.Store(long_entry));

  ASSERT_OK_AND_ASSIGN(
      ProjectionPrefixCacheLookup lookup,
      cache.FindLongestPrefixHit(Identity(), kSchemaJson, prompt_three));
  EXPECT_EQ(lookup.entry.event_count, 2);
  EXPECT_EQ(lookup.entry.checkpoint_manifest_hash, FilledHash(0x32));
  EXPECT_EQ(lookup.hit.suffix, prompt_three.substr(prompt_two.size()));
}

}  // namespace
}  // namespace litert::lm
