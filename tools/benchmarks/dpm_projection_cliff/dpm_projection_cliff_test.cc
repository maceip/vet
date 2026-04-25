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

#include "tools/benchmarks/dpm_projection_cliff/dpm_projection_cliff.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace litert::lm::bench {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

TEST(CliffRowToJsonlTest, EmitsRequiredFieldsAndOmitsAbsentOptionals) {
  CliffRow row;
  row.condition = "dpm_projection";
  row.trajectory_chars = 27000;
  row.memory_budget_chars = 5352;
  row.repeat_idx = 3;
  row.architecture_tag = "arm64-hexagon-int8";
  row.manifest_hash = "ab12";
  row.runtime_version = "litertlm-1.2.3";
  row.kv_dtype = "fp16";
  row.model_class = "gqa";
  row.decision_score = 0.93;
  row.wall_clock_decision_ms = 80.5;
  row.wall_clock_append_p50_us = 50.0;
  row.wall_clock_append_p99_us = 1000.0;
  row.disk_bytes_session_total = 200000;
  row.disk_bytes_event_log = 180000;

  const std::string s = CliffRowToJsonl(row);
  EXPECT_THAT(s, HasSubstr("\"schema_version\":1"));
  EXPECT_THAT(s, HasSubstr("\"condition\":\"dpm_projection\""));
  EXPECT_THAT(s, HasSubstr("\"trajectory_chars\":27000"));
  EXPECT_THAT(s, HasSubstr("\"kv_dtype\":\"fp16\""));
  EXPECT_THAT(s, HasSubstr("\"kv_dtype_policy_replay_safe\":true"));
  // Optional fields absent by default => omitted.
  EXPECT_THAT(s, Not(HasSubstr("wall_clock_checkpoint_put_ms")));
  EXPECT_THAT(s, Not(HasSubstr("wall_clock_thaw_ms")));
  EXPECT_THAT(s, Not(HasSubstr("kv_bytes_per_1024_tokens")));
  EXPECT_THAT(s, Not(HasSubstr("must_refill_from_log")));
  EXPECT_THAT(s, Not(HasSubstr("\"mock\"")));
}

TEST(CliffRowToJsonlTest, EmitsOptionalsWhenSet) {
  CliffRow row;
  row.condition = "dpm_checkpoints";
  row.wall_clock_checkpoint_put_ms = 120.0;
  row.wall_clock_thaw_ms = 22.0;
  row.kv_bytes_per_1024_tokens = 600000;
  row.must_refill_from_log = true;
  row.tamper_test_json =
      R"json({"scenario":"manifest_hash_mismatch_artifact_hash"})json";
  row.mock = true;

  const std::string s = CliffRowToJsonl(row);
  EXPECT_THAT(s, HasSubstr("\"wall_clock_checkpoint_put_ms\":120"));
  EXPECT_THAT(s, HasSubstr("\"wall_clock_thaw_ms\":22"));
  EXPECT_THAT(s, HasSubstr("\"kv_bytes_per_1024_tokens\":600000"));
  EXPECT_THAT(s, HasSubstr("\"must_refill_from_log\":true"));
  EXPECT_THAT(s, HasSubstr("\"tamper_test\":{\"scenario\":"));
  EXPECT_THAT(s, HasSubstr("\"mock\":true"));
}

TEST(CliffRowToJsonlTest, EscapesControlCharacters) {
  CliffRow row;
  row.condition = "dpm_projection";
  row.architecture_tag = "rig\nwith\tweird\"name";
  const std::string s = CliffRowToJsonl(row);
  EXPECT_THAT(s, HasSubstr("\\n"));
  EXPECT_THAT(s, HasSubstr("\\t"));
  EXPECT_THAT(s, HasSubstr("\\\""));
}

TEST(LoadConfigTest, ParsesScalarsAndLists) {
  const std::filesystem::path tmp =
      std::filesystem::temp_directory_path() / "cliff_cfg.yaml";
  {
    std::ofstream out(tmp);
    out << "name: test_sweep\n"
        << "seed: 12345\n"
        << "repeats: 3\n"
        << "conditions:\n"
        << "  - summarization_baseline\n"
        << "  - dpm_projection\n"
        << "trajectory_chars:\n"
        << "  - 1000\n"
        << "  - 27000\n"
        << "memory_budget_chars:\n"
        << "  - 5352\n"
        << "model_class: gqa\n"
        << "kv_dtype: fp16\n"
        << "kv_dtype_policy_replay_safe: true\n";
  }
  auto cfg = LoadConfig(tmp.string());
  std::filesystem::remove(tmp);
  ASSERT_TRUE(cfg.ok()) << cfg.status();
  EXPECT_EQ(cfg->name, "test_sweep");
  EXPECT_EQ(cfg->seed, 12345u);
  EXPECT_EQ(cfg->repeats, 3u);
  ASSERT_EQ(cfg->conditions.size(), 2);
  EXPECT_EQ(cfg->conditions[0], "summarization_baseline");
  EXPECT_EQ(cfg->conditions[1], "dpm_projection");
  ASSERT_EQ(cfg->trajectory_chars.size(), 2);
  EXPECT_EQ(cfg->trajectory_chars[0], 1000u);
  EXPECT_EQ(cfg->trajectory_chars[1], 27000u);
  ASSERT_EQ(cfg->memory_budget_chars.size(), 1);
  EXPECT_EQ(cfg->memory_budget_chars[0], 5352u);
  EXPECT_EQ(cfg->model_class, "gqa");
  EXPECT_EQ(cfg->kv_dtype, "fp16");
  EXPECT_TRUE(cfg->kv_dtype_policy_replay_safe);
}

TEST(LoadConfigTest, ToleratesUnknownKeysAndComments) {
  const std::filesystem::path tmp =
      std::filesystem::temp_directory_path() / "cliff_cfg_tolerant.yaml";
  {
    std::ofstream out(tmp);
    out << "# leading comment\n"
        << "name: tolerant\n"
        << "future_field: ignored # inline comment\n"
        << "future_list:\n"
        << "  - a\n"
        << "  - b\n"
        << "trajectory_chars:\n"
        << "  - 100\n"
        << "memory_budget_chars:\n"
        << "  - 4096\n"
        << "conditions:\n"
        << "  - dpm_projection\n";
  }
  auto cfg = LoadConfig(tmp.string());
  std::filesystem::remove(tmp);
  ASSERT_TRUE(cfg.ok());
  EXPECT_EQ(cfg->name, "tolerant");
  EXPECT_EQ(cfg->trajectory_chars.size(), 1);
}

TEST(LoadConfigTest, MissingFileReturnsNotFound) {
  auto cfg = LoadConfig("/no/such/file.yaml");
  EXPECT_EQ(cfg.status().code(), absl::StatusCode::kNotFound);
}

TEST(RunOneCellTest, MockReturnsConsistentRowForFixedSeed) {
  CliffConfig cfg;
  cfg.seed = 20260420;
  cfg.kv_dtype = "fp16";
  cfg.kv_dtype_policy_replay_safe = true;
  cfg.model_class = "gqa";
  auto a = RunOneCell(cfg, "dpm_projection", 27000, 5352, 0);
  auto b = RunOneCell(cfg, "dpm_projection", 27000, 5352, 0);
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(a->decision_score, b->decision_score);
  EXPECT_TRUE(a->mock);
}

TEST(RunOneCellTest, MockCheckpointsConditionEmitsPutTime) {
  CliffConfig cfg;
  cfg.seed = 20260420;
  auto row = RunOneCell(cfg, "dpm_checkpoints", 27000, 5352, 0);
  ASSERT_TRUE(row.ok());
  EXPECT_TRUE(row->wall_clock_checkpoint_put_ms.has_value());
}

}  // namespace
}  // namespace litert::lm::bench
