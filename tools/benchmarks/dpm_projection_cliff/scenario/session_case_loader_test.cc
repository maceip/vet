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

#include "tools/benchmarks/dpm_projection_cliff/scenario/session_case_loader.h"

#include <string>
#include <vector>

#include "absl/strings/match.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gmock/gmock.h"  // from @com_google_googletest
#include "gtest/gtest.h"  // from @com_google_googletest

namespace litert::lm::bench {
namespace {

using ::testing::HasSubstr;

constexpr absl::string_view kMinimalSingleCase = R"json({
  "case_id": "test-case-1",
  "domain": "claude",
  "source_path": "/dev/null",
  "source_sha256": "deadbeef",
  "n_events": 2,
  "probe_T": 1,
  "events": [
    {"idx": 0, "kind": "user", "role": "user", "text": "hello",
     "timestamp": "2026-04-29T00:00:00Z"},
    {"idx": 1, "kind": "assistant_text", "role": "assistant",
     "text": "hi there", "timestamp": "2026-04-29T00:00:01Z"}
  ],
  "probes": [
    {"kind": "next_user_intent",
     "question": "what does the user say next?",
     "expected_match": {"substring": "follow-up question"},
     "rationale": "test"}
  ]
})json";

constexpr absl::string_view kArrayOfTwo = R"json([
  {"case_id":"a","domain":"claude","source_path":"","source_sha256":"",
   "n_events":1,"probe_T":0,
   "events":[{"idx":0,"kind":"user","role":"user","text":"x",
              "timestamp":""}],
   "probes":[{"kind":"next_user_intent","question":"q",
              "expected_match":{"substring":"y"},"rationale":""}]},
  {"case_id":"b","domain":"codex","source_path":"","source_sha256":"",
   "n_events":1,"probe_T":0,
   "events":[{"idx":0,"kind":"user","role":"user","text":"x",
              "timestamp":""}],
   "probes":[{"kind":"next_tool_call","question":"q",
              "expected_match":{"tool_name":"Read",
                                "arg_substring":"path"},
              "rationale":""}]}
])json";

TEST(SessionCaseLoaderTest, ParsesSingleObject) {
  auto cases = ParseSessionCases(kMinimalSingleCase);
  ASSERT_TRUE(cases.ok()) << cases.status();
  ASSERT_EQ(cases->size(), 1u);
  const auto& c = (*cases)[0];
  EXPECT_EQ(c.case_id, "test-case-1");
  EXPECT_EQ(c.domain, "claude");
  EXPECT_EQ(c.n_events, 2);
  EXPECT_EQ(c.probe_T, 1);
  ASSERT_EQ(c.events.size(), 2u);
  EXPECT_EQ(c.events[0].kind, "user");
  EXPECT_EQ(c.events[1].text, "hi there");
  ASSERT_EQ(c.probes.size(), 1u);
  EXPECT_EQ(c.probes[0].kind, "next_user_intent");
  EXPECT_EQ(c.probes[0].expected_match.substring, "follow-up question");
}

TEST(SessionCaseLoaderTest, ParsesArrayOfMany) {
  auto cases = ParseSessionCases(kArrayOfTwo);
  ASSERT_TRUE(cases.ok()) << cases.status();
  ASSERT_EQ(cases->size(), 2u);
  EXPECT_EQ((*cases)[0].case_id, "a");
  EXPECT_EQ((*cases)[1].case_id, "b");
  EXPECT_EQ((*cases)[1].probes[0].expected_match.tool_name, "Read");
  EXPECT_EQ((*cases)[1].probes[0].expected_match.arg_substring, "path");
}

TEST(SessionCaseLoaderTest, RejectsMissingCaseId) {
  constexpr absl::string_view bad = R"json({"events":[],"probes":[]})json";
  auto r = ParseSessionCases(bad);
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(r.status().message(), HasSubstr("case_id"));
}

TEST(SessionCaseLoaderTest, RejectsInvalidJson) {
  auto r = ParseSessionCases("{not json");
  ASSERT_FALSE(r.ok());
  EXPECT_THAT(r.status().message(), HasSubstr("parse error"));
}

TEST(SessionCaseLoaderTest, RendersEventLogMatchesIngesterShape) {
  auto cases = ParseSessionCases(kMinimalSingleCase);
  ASSERT_TRUE(cases.ok());
  std::string log = RenderEventLog((*cases)[0]);
  // Each event should appear on its own line, prefixed with [N+1]
  EXPECT_THAT(log, HasSubstr("[1] hello\n"));
  EXPECT_THAT(log, HasSubstr("[2] hi there\n"));
}

TEST(SessionCaseLoaderTest, ParsesGoldenSeedCase) {
  // Sanity: the checked-in golden seed case must always load cleanly.
  // If this fails, future schema drift broke the loader; either bump
  // the loader or refresh the golden.
  auto cases = LoadSessionCasesFromFile(
      "tools/benchmarks/dpm_projection_cliff/scenario/golden/seed_case.json");
  ASSERT_TRUE(cases.ok()) << cases.status();
  ASSERT_EQ(cases->size(), 1u);
  EXPECT_EQ((*cases)[0].case_id, "seed-redacted-2026-04-29");
  EXPECT_EQ((*cases)[0].events.size(), 12u);
  EXPECT_EQ((*cases)[0].probes.size(), 3u);
}

}  // namespace
}  // namespace litert::lm::bench
