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

#include "runtime/dpm/dpm_projector.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "nlohmann/json.hpp"  // from @nlohmann_json
#include "runtime/dpm/event.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/dpm/projection_prompt.h"
#include "runtime/proto/sampler_params.pb.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {
namespace {

bool HasCitationAnchor(absl::string_view text) {
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '[') {
      continue;
    }
    size_t j = i + 1;
    bool has_digit = false;
    bool has_nonzero_digit = false;
    while (j < text.size() && text[j] >= '0' && text[j] <= '9') {
      has_digit = true;
      has_nonzero_digit = has_nonzero_digit || text[j] != '0';
      ++j;
    }
    if (has_digit && has_nonzero_digit && j < text.size() && text[j] == ']') {
      return true;
    }
  }
  return false;
}

absl::Status ValidateProjectionCitations(const nlohmann::ordered_json& json) {
  if (!json.is_object()) {
    return absl::InvalidArgumentError(
        "DPM projection output must be a JSON object.");
  }
  static constexpr absl::string_view kRequiredFields[] = {
      "Facts", "Reasoning", "Compliance"};
  for (absl::string_view field : kRequiredFields) {
    const std::string field_name(field);
    if (!json.contains(field_name)) {
      return absl::InvalidArgumentError(
          absl::StrCat("DPM projection output is missing field ", field));
    }
    if (!json[field_name].is_array()) {
      return absl::InvalidArgumentError(
          absl::StrCat("DPM projection field ", field, " must be an array."));
    }
    for (const nlohmann::ordered_json& item : json[field_name]) {
      const std::string item_text =
          item.is_string() ? item.get<std::string>() : item.dump();
      if (!HasCitationAnchor(item_text)) {
        return absl::InvalidArgumentError(absl::StrCat(
            "DPM projection item in ", field,
            " is missing a one-based Event Index citation like [1]."));
      }
    }
  }
  return absl::OkStatus();
}

}  // namespace

proto::SamplerParameters CreateDPMSamplerParameters(
    const DPMInferenceConfig& config) {
  proto::SamplerParameters params;
  params.set_type(proto::SamplerParameters::TOP_P);
  params.set_k(1);
  params.set_p(1.0f);
  params.set_temperature(config.temperature);
  params.set_seed(config.seed);
  return params;
}

DPMProjector::DPMProjector(DPMInferenceRunner* runner) : runner_(runner) {}

absl::StatusOr<std::string> DPMProjector::Project(
    const EventSourcedLog& log, const ProjectionConfig& config) {
  if (runner_ == nullptr) {
    return absl::FailedPreconditionError("DPM projector runner is null.");
  }
  if (config.model_id.empty()) {
    return absl::InvalidArgumentError(
        "DPM projection requires a pinned model_id.");
  }
  ASSIGN_OR_RETURN(std::string prompt, CreateProjectionPrompt(log, config));
  ASSIGN_OR_RETURN(
      std::string raw_projection,
      runner_->Generate(prompt,
                        DPMInferenceConfig{
                            .max_output_tokens =
                                static_cast<int>(config.max_tokens),
                            .seed = config.seed,
                            .temperature = config.temperature,
                            .fresh_context = true,
                            .model_id = config.model_id,
                        }));
  try {
    nlohmann::ordered_json projection =
        nlohmann::ordered_json::parse(raw_projection);
    RETURN_IF_ERROR(ValidateProjectionCitations(projection));
    return projection.dump();
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(
        absl::StrCat("DPM projection output is not valid JSON: ", e.what(),
                     "; raw prefix: ",
                     raw_projection.substr(0, 512)));
  }
}

absl::StatusOr<std::string> DPMProjector::ProjectRange(
    const EventSourcedLog& log, uint64_t event_range_start,
    uint64_t event_range_end, const ProjectionConfig& config) {
  if (runner_ == nullptr) {
    return absl::FailedPreconditionError("DPM projector runner is null.");
  }
  if (config.model_id.empty()) {
    return absl::InvalidArgumentError(
        "DPM projection requires a pinned model_id.");
  }
  ASSIGN_OR_RETURN(std::string prompt,
                   CreateProjectionPromptForRange(log, event_range_start,
                                                  event_range_end, config));
  ASSIGN_OR_RETURN(
      std::string raw_projection,
      runner_->Generate(prompt,
                        DPMInferenceConfig{
                            .max_output_tokens =
                                static_cast<int>(config.max_tokens),
                            .seed = config.seed,
                            .temperature = config.temperature,
                            .fresh_context = true,
                            .model_id = config.model_id,
                        }));
  try {
    nlohmann::ordered_json projection =
        nlohmann::ordered_json::parse(raw_projection);
    RETURN_IF_ERROR(ValidateProjectionCitations(projection));
    return projection.dump();
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(
        absl::StrCat("DPM projection output is not valid JSON: ", e.what(),
                     "; raw prefix: ",
                     raw_projection.substr(0, 512)));
  }
}

absl::StatusOr<std::string> DPMProjector::CreateProjectionPrompt(
    const EventSourcedLog& log, const ProjectionConfig& config) const {
  if (config.schema_id.empty()) {
    return absl::InvalidArgumentError(
        "DPM projection requires a non-empty schema_id.");
  }
  if (config.schema_json.empty()) {
    return absl::InvalidArgumentError(
        "DPM projection requires a non-empty task schema.");
  }
  try {
    (void)nlohmann::ordered_json::parse(config.schema_json);
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(
        absl::StrCat("DPM projection schema is not valid JSON: ", e.what()));
  }
  ASSIGN_OR_RETURN(std::string event_log, log.GetProjectionEventLog());
  ASSIGN_OR_RETURN(
      std::string prompt,
      litert::lm::CreateProjectionPrompt(
          event_log, config.schema_id, config.schema_json,
          config.memory_budget_chars, config.max_event_log_chars));
  return prompt;
}

absl::StatusOr<std::string> DPMProjector::CreateProjectionPromptForRange(
    const EventSourcedLog& log, uint64_t event_range_start,
    uint64_t event_range_end, const ProjectionConfig& config) const {
  if (config.schema_id.empty()) {
    return absl::InvalidArgumentError(
        "DPM projection requires a non-empty schema_id.");
  }
  if (config.schema_json.empty()) {
    return absl::InvalidArgumentError(
        "DPM projection requires a non-empty task schema.");
  }
  try {
    (void)nlohmann::ordered_json::parse(config.schema_json);
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(
        absl::StrCat("DPM projection schema is not valid JSON: ", e.what()));
  }
  ASSIGN_OR_RETURN(std::string event_log,
                   log.GetProjectionEventLogRange(event_range_start,
                                                  event_range_end));
  ASSIGN_OR_RETURN(
      std::string prompt,
      litert::lm::CreateProjectionPrompt(
          event_log, config.schema_id, config.schema_json,
          config.memory_budget_chars, config.max_event_log_chars));
  return prompt;
}

}  // namespace litert::lm
