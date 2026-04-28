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
  row.frp = 0.93;
  row.rcs = 0.91;
  row.eda = 1.0;
  row.crr = 0.95;
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
  // Per-axis fields are emitted when set.
  EXPECT_THAT(s, HasSubstr("\"frp\":0.93"));
  EXPECT_THAT(s, HasSubstr("\"rcs\":0.91"));
  EXPECT_THAT(s, HasSubstr("\"eda\":1"));
  EXPECT_THAT(s, HasSubstr("\"crr\":0.95"));
  // Composite decision_score is optional/derived; absent here.
  EXPECT_THAT(s, Not(HasSubstr("\"decision_score\"")));
  EXPECT_THAT(s, Not(HasSubstr("\"deterministic_score\"")));
  // Other optional fields absent by default => omitted.
  EXPECT_THAT(s, Not(HasSubstr("wall_clock_checkpoint_put_ms")));
  EXPECT_THAT(s, Not(HasSubstr("wall_clock_thaw_ms")));
  EXPECT_THAT(s, Not(HasSubstr("kv_bytes_per_1024_tokens")));
  EXPECT_THAT(s, Not(HasSubstr("must_refill_from_log")));
  EXPECT_THAT(s, Not(HasSubstr("\"mock\"")));
  // Empty provenance fields are omitted (caller hadn't filled them).
  EXPECT_THAT(s, Not(HasSubstr("\"hostname\"")));
  EXPECT_THAT(s, Not(HasSubstr("\"git_sha\"")));
  EXPECT_THAT(s, Not(HasSubstr("\"config_hash\"")));
}

TEST(CliffRowToJsonlTest, EmitsPreJudgeCorpusScoringFields) {
  CliffRow row;
  row.condition = "dpm_projection";
  row.evidence_lane = "quality";
  row.claim_tested = "paper_dpm_projection";
  row.frp = 1.0;
  row.eda = 1.0;
  row.crr = 0.0;
  row.deterministic_score = 2.0 / 3.0;
  row.scored_axis_count = 3;
  row.pending_judge_axes = "rcs";
  row.wall_clock_memory_build_ms = 123.5;

  const std::string s = CliffRowToJsonl(row);
  EXPECT_THAT(s, HasSubstr("\"deterministic_score\":0.666667"));
  EXPECT_THAT(s, HasSubstr("\"scored_axis_count\":3"));
  EXPECT_THAT(s, HasSubstr("\"pending_judge_axes\":\"rcs\""));
  EXPECT_THAT(s, HasSubstr("\"evidence_lane\":\"quality\""));
  EXPECT_THAT(s, HasSubstr("\"claim_tested\":\"paper_dpm_projection\""));
  EXPECT_THAT(s, HasSubstr("\"wall_clock_memory_build_ms\":123.5"));
  EXPECT_THAT(s, Not(HasSubstr("\"decision_score\"")));
}

TEST(CliffRowToJsonlTest, EmitsOptionalsWhenSet) {
  CliffRow row;
  row.condition = "dpm_checkpoints";
  row.decision_score = 0.92;
  row.wall_clock_checkpoint_put_ms = 120.0;
  row.wall_clock_thaw_ms = 22.0;
  row.kv_bytes_per_1024_tokens = 600000;
  row.must_refill_from_log = true;
  // Provenance.
  row.config_hash = "cfg-hash";
  row.git_sha = "abcdef";
  row.dirty_tree = false;
  row.hostname = "rig-r2";
  row.os = "Linux 6.8.0";
  row.cpu_model = "Snapdragon X Elite";
  row.accelerator_id = "hexagon-v75";
  row.tamper_test_json =
      R"json({"scenario":"manifest_hash_mismatch_artifact_hash"})json";
  row.mock = true;

  const std::string s = CliffRowToJsonl(row);
  EXPECT_THAT(s, HasSubstr("\"decision_score\":0.92"));
  EXPECT_THAT(s, HasSubstr("\"wall_clock_checkpoint_put_ms\":120"));
  EXPECT_THAT(s, HasSubstr("\"wall_clock_thaw_ms\":22"));
  EXPECT_THAT(s, HasSubstr("\"kv_bytes_per_1024_tokens\":600000"));
  EXPECT_THAT(s, HasSubstr("\"must_refill_from_log\":true"));
  EXPECT_THAT(s, HasSubstr("\"config_hash\":\"cfg-hash\""));
  EXPECT_THAT(s, HasSubstr("\"git_sha\":\"abcdef\""));
  EXPECT_THAT(s, HasSubstr("\"dirty_tree\":false"));
  EXPECT_THAT(s, HasSubstr("\"hostname\":\"rig-r2\""));
  EXPECT_THAT(s, HasSubstr("\"accelerator_id\":\"hexagon-v75\""));
  EXPECT_THAT(s, HasSubstr("\"tamper_test\":{\"scenario\":"));
  EXPECT_THAT(s, HasSubstr("\"mock\":true"));
}

TEST(RunOneCellTest, RefusesEmptyModelWithoutAllowMock) {
  // With model_path empty and allow_mock off, the driver fails closed
  // so a stale build cannot silently produce mock rows. Real runs pass
  // a non-empty model_path; pure-mock runs flip allow_mock=true.
  CliffConfig cfg;
  cfg.seed = 20260420;
  cfg.allow_mock = false;
  cfg.model_path.clear();
  auto status = RunOneCell(cfg, "dpm_projection", 27000, 5352, 0);
  EXPECT_EQ(status.status().code(),
            absl::StatusCode::kFailedPrecondition);
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
  cfg.allow_mock = true;
  auto a = RunOneCell(cfg, "dpm_projection", 27000, 5352, 0);
  auto b = RunOneCell(cfg, "dpm_projection", 27000, 5352, 0);
  ASSERT_TRUE(a.ok()) << a.status();
  ASSERT_TRUE(b.ok()) << b.status();
  EXPECT_EQ(a->frp, b->frp);
  EXPECT_EQ(a->rcs, b->rcs);
  EXPECT_TRUE(a->mock);
}

TEST(RunOneCellTest, MockCheckpointsConditionEmitsPutTime) {
  CliffConfig cfg;
  cfg.seed = 20260420;
  cfg.allow_mock = true;
  auto row = RunOneCell(cfg, "dpm_checkpoints", 27000, 5352, 0);
  ASSERT_TRUE(row.ok());
  EXPECT_TRUE(row->wall_clock_checkpoint_put_ms.has_value());
}

}  // namespace
}  // namespace litert::lm::bench
