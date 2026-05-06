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

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/match.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gtest/gtest.h"
#include "runtime/dpm/dpm_projector.h"
#include "runtime/dpm/engine_inference_runner.h"
#include "runtime/dpm/event.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/engine/engine.h"
#include "runtime/engine/engine_factory.h"
#include "runtime/engine/engine_settings.h"
#include "runtime/executor/executor_settings_base.h"
#include "runtime/util/status_macros.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

constexpr absl::string_view kEnableEnv =
    "LITERTLM_ENABLE_E2E_DETERMINISM_TEST";
constexpr absl::string_view kModelPathEnv = "DPM_DETERMINISM_MODEL_PATH";
constexpr absl::string_view kModelIdEnv = "DPM_DETERMINISM_MODEL_ID";
constexpr absl::string_view kBackendEnv = "DPM_DETERMINISM_BACKEND";
constexpr absl::string_view kSchemaJson =
    R"json({
      "Facts":["string with one-based [i] citation"],
      "Reasoning":["string with one-based [i] citation"],
      "Compliance":["string with one-based [i] citation"]
    })json";

std::string GetEnv(absl::string_view name) {
  const char* value = std::getenv(std::string(name).c_str());
  return value == nullptr ? "" : std::string(value);
}

bool EnvFlagEnabled(absl::string_view name) {
  const std::string value = GetEnv(name);
  return value == "1" || absl::EqualsIgnoreCase(value, "true") ||
         absl::EqualsIgnoreCase(value, "yes");
}

std::filesystem::path TestPath(absl::string_view name) {
  std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / std::string(name);
  std::filesystem::remove_all(path);
  return path;
}

void AppendDeterminismFixture(EventSourcedLog* log) {
  ASSERT_OK(log->Append(Event{
      .type = Event::Type::kUser,
      .payload =
          "Claimant reports rear-impact collision on 2026-04-02 with "
          "$4,812.40 estimated bumper damage.",
      .timestamp_us = 100,
  }));
  ASSERT_OK(log->Append(Event{
      .type = Event::Type::kTool,
      .payload =
          "Policy POL-88 has collision coverage, deductible $500, rental "
          "limit $40/day, and no lapsed-payment warning.",
      .timestamp_us = 200,
  }));
  ASSERT_OK(log->Append(Event{
      .type = Event::Type::kCorrection,
      .payload =
          "Correction: accident date is 2026-04-03; all dollar amounts remain "
          "unchanged.",
      .timestamp_us = 300,
  }));
}

absl::StatusOr<std::unique_ptr<Engine>> CreatePinnedEngine(
    absl::string_view model_path, Backend backend) {
  ASSIGN_OR_RETURN(ModelAssets model_assets, ModelAssets::Create(model_path));
  ASSIGN_OR_RETURN(EngineSettings engine_settings,
                   EngineSettings::CreateDefault(std::move(model_assets),
                                                 backend));
  return EngineFactory::CreateDefault(std::move(engine_settings));
}

TEST(DPMDeterminismE2ETest, PinnedModelProjectionIsByteIdenticalAcrossReplays) {
  if (!EnvFlagEnabled(kEnableEnv)) {
    GTEST_SKIP() << "Set " << kEnableEnv
                 << "=1 to run the pinned-model determinism e2e.";
  }
  const std::string model_path = GetEnv(kModelPathEnv);
  if (model_path.empty()) {
    GTEST_SKIP() << "Set " << kModelPathEnv
                 << " to the pinned .litertlm model artifact.";
  }
  const std::string backend_name =
      GetEnv(kBackendEnv).empty() ? "cpu" : GetEnv(kBackendEnv);
  ASSERT_OK_AND_ASSIGN(Backend backend, GetBackendFromString(backend_name));
  const std::string model_id =
      GetEnv(kModelIdEnv).empty()
          ? absl::StrCat("pinned-local:", std::filesystem::path(model_path)
                                               .filename()
                                               .string())
          : GetEnv(kModelIdEnv);

  ASSERT_OK_AND_ASSIGN(std::unique_ptr<Engine> engine,
                       CreatePinnedEngine(model_path, backend));
  EngineDPMInferenceRunner runner(engine.get(), SessionConfig::CreateDefault());
  DPMProjector projector(&runner);

  EventSourcedLog log(TestPath("dpm_determinism_e2e"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  AppendDeterminismFixture(&log);

  DPMProjector::ProjectionConfig config;
  config.max_tokens = 512;
  config.memory_budget_chars = 1338;
  config.max_event_log_chars = 32768;
  config.schema_id = "insurance_liability_v2";
  config.schema_json = std::string(kSchemaJson);
  config.model_id = model_id;

  std::string first_projection;
  for (int replay = 0; replay < 10; ++replay) {
    SCOPED_TRACE(absl::StrCat("replay=", replay));
    ASSERT_OK_AND_ASSIGN(std::string projection,
                         projector.Project(log, config));
    if (replay == 0) {
      first_projection = projection;
    }
    EXPECT_EQ(projection, first_projection);
  }
}

}  // namespace
}  // namespace litert::lm
