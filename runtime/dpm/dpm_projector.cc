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
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/str_join.h"  // from @com_google_absl
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

absl::StatusOr<std::string> CanonicalizeProjectionJson(
    absl::string_view raw_projection) {
  try {
    nlohmann::ordered_json projection =
        nlohmann::ordered_json::parse(std::string(raw_projection));
    RETURN_IF_ERROR(ValidateProjectionCitations(projection));
    return projection.dump();
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(absl::StrCat(
        "DPM projection output is not valid JSON: ", e.what(),
        "; raw prefix: ", std::string(raw_projection).substr(0, 512)));
  }
}

DPMInferenceConfig InferenceConfigFor(
    const DPMProjector::ProjectionConfig& config) {
  return DPMInferenceConfig{
      .max_output_tokens = static_cast<int>(config.max_tokens),
      .seed = config.seed,
      .temperature = config.temperature,
      .fresh_context = true,
      .model_id = config.model_id,
  };
}

std::string LowerAscii(absl::string_view text) {
  std::string out(text);
  for (char& ch : out) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return out;
}

bool CorrectionAppliesToEvent(uint64_t event_index,
                              const ProjectionCorrectionDirective& directive) {
  switch (directive.scope) {
    case ProjectionCorrectionScope::kPriorEvents:
      return event_index < directive.correction_event_index;
    case ProjectionCorrectionScope::kCheckpointRange:
    case ProjectionCorrectionScope::kGlobal:
      return true;
  }
  return true;
}

std::vector<std::string> InvalidatedFactsForEvent(
    const Event& event, uint64_t event_index,
    const std::vector<ProjectionCorrectionDirective>& directives) {
  std::vector<std::string> hits;
  if (event.type == Event::Type::kCorrection) return hits;
  const std::string lowered_payload = LowerAscii(event.payload);
  for (const ProjectionCorrectionDirective& directive : directives) {
    if (!CorrectionAppliesToEvent(event_index, directive)) continue;
    for (const std::string& fact : directive.invalidated_facts) {
      if (fact.empty()) continue;
      if (lowered_payload.find(LowerAscii(fact)) == std::string::npos) {
        continue;
      }
      hits.push_back(fact);
    }
  }
  return hits;
}

std::vector<std::string> CorrectionIdsForEvent(
    const Event& event, uint64_t event_index,
    const std::vector<ProjectionCorrectionDirective>& directives) {
  std::vector<std::string> correction_ids;
  if (event.type == Event::Type::kCorrection) return correction_ids;
  const std::string lowered_payload = LowerAscii(event.payload);
  for (const ProjectionCorrectionDirective& directive : directives) {
    if (!CorrectionAppliesToEvent(event_index, directive)) continue;
    bool matched = false;
    for (const std::string& fact : directive.invalidated_facts) {
      if (fact.empty()) continue;
      if (lowered_payload.find(LowerAscii(fact)) != std::string::npos) {
        matched = true;
        break;
      }
    }
    if (matched) {
      correction_ids.push_back(directive.correction_event_id.empty()
                                   ? "unknown"
                                   : directive.correction_event_id);
    }
  }
  return correction_ids;
}

Event RevokedEvidenceEvent(
    const Event& event, uint64_t event_index,
    const std::vector<ProjectionCorrectionDirective>& directives) {
  const std::vector<std::string> facts =
      InvalidatedFactsForEvent(event, event_index, directives);
  const std::vector<std::string> correction_ids =
      CorrectionIdsForEvent(event, event_index, directives);
  Event revoked = event;
  revoked.type = Event::Type::kInternal;
  revoked.payload = absl::StrCat(
      "REVOKED_BY_CORRECTION: this event contained ", facts.size(),
      " invalidated fact(s) and is not active evidence for the projection. "
      "Apply correction_id(s): ",
      correction_ids.empty() ? "unknown" : absl::StrJoin(correction_ids, ","),
      ". Do not reconstruct the original revoked wording from this marker.");
  return revoked;
}

absl::StatusOr<std::string> GetCorrectionAppliedProjectionEventLogRange(
    const EventSourcedLog& log, uint64_t event_range_start,
    uint64_t event_range_end,
    const std::vector<ProjectionCorrectionDirective>& correction_directives) {
  if (event_range_end < event_range_start) {
    return absl::InvalidArgumentError("DPM projection event range is inverted.");
  }
  ASSIGN_OR_RETURN(std::vector<Event> events, log.GetAllEvents());
  if (event_range_end > events.size()) {
    return absl::InvalidArgumentError(
        "DPM projection event range exceeds log generation.");
  }
  std::string event_log;
  for (uint64_t i = event_range_start; i < event_range_end; ++i) {
    if (!event_log.empty()) event_log.push_back('\n');
    const Event& event = events[static_cast<size_t>(i)];
    const std::vector<std::string> hits =
        InvalidatedFactsForEvent(event, i, correction_directives);
    const Event rendered =
        hits.empty() ? event
                     : RevokedEvidenceEvent(event, i, correction_directives);
    absl::StrAppend(&event_log, "[", i + 1, "] ",
                    EventToJsonLine(rendered));
  }
  return event_log;
}

std::string CreateCorrectionRepairPrompt(
    absl::string_view previous_projection,
    const std::vector<std::string>& invalidated_facts,
    const std::vector<ProjectionCorrectionDirective>& correction_directives) {
  return absl::StrCat(
      "System. CORRECTION REPAIR for DPM projected memory.\n"
      "The previous projection violated blocking correction directives. "
      "Remove the forbidden facts listed below while preserving valid JSON, "
      "one-based citations, and all non-conflicting replacement facts.\n\n",
      FormatProjectionCorrectionDirectives(correction_directives),
      "[FORBIDDEN SUBSTRINGS]\n- ", absl::StrJoin(invalidated_facts, "\n- "),
      "\n\n[PREVIOUS PROJECTION]\n", previous_projection, "\n\n",
      "[EXPECTED OUTPUT]\n"
      "Return only a valid JSON object in this exact shape:\n",
      "{\"Facts\":[\"... [i]\"],\"Reasoning\":[\"... [i]\"],"
      "\"Compliance\":[\"... [i]\"]}\n",
      "The corrected JSON must not contain any forbidden substring.\n");
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
      runner_->Generate(prompt, InferenceConfigFor(config)));
  return CanonicalizeProjectionJson(raw_projection);
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
      runner_->Generate(prompt, InferenceConfigFor(config)));
  return CanonicalizeProjectionJson(raw_projection);
}

absl::StatusOr<std::string> DPMProjector::ProjectWithCorrections(
    const EventSourcedLog& log, const ProjectionConfig& config,
    const std::vector<ProjectionCorrectionDirective>& correction_directives) {
  if (runner_ == nullptr) {
    return absl::FailedPreconditionError("DPM projector runner is null.");
  }
  if (config.model_id.empty()) {
    return absl::InvalidArgumentError(
        "DPM projection requires a pinned model_id.");
  }
  ASSIGN_OR_RETURN(
      std::string prompt,
      CreateProjectionPromptWithCorrections(log, config,
                                            correction_directives));
  ASSIGN_OR_RETURN(
      std::string raw_projection,
      runner_->Generate(prompt, InferenceConfigFor(config)));
  ASSIGN_OR_RETURN(std::string projection,
                   CanonicalizeProjectionJson(raw_projection));
  std::vector<std::string> invalidated =
      FindInvalidatedFacts(projection, correction_directives);
  for (int attempt = 0;
       !invalidated.empty() && attempt < config.correction_repair_attempts;
       ++attempt) {
    ASSIGN_OR_RETURN(
        std::string repaired_raw,
        runner_->Generate(CreateCorrectionRepairPrompt(
                              projection, invalidated, correction_directives),
                          InferenceConfigFor(config)));
    ASSIGN_OR_RETURN(projection, CanonicalizeProjectionJson(repaired_raw));
    invalidated = FindInvalidatedFacts(projection, correction_directives);
  }
  RETURN_IF_ERROR(
      ValidateNoInvalidatedFacts(projection, correction_directives));
  return projection;
}

absl::StatusOr<std::string> DPMProjector::ProjectRangeWithCorrections(
    const EventSourcedLog& log, uint64_t event_range_start,
    uint64_t event_range_end, const ProjectionConfig& config,
    const std::vector<ProjectionCorrectionDirective>& correction_directives) {
  if (runner_ == nullptr) {
    return absl::FailedPreconditionError("DPM projector runner is null.");
  }
  if (config.model_id.empty()) {
    return absl::InvalidArgumentError(
        "DPM projection requires a pinned model_id.");
  }
  ASSIGN_OR_RETURN(
      std::string prompt,
      CreateProjectionPromptForRangeWithCorrections(
          log, event_range_start, event_range_end, config,
          correction_directives));
  ASSIGN_OR_RETURN(
      std::string raw_projection,
      runner_->Generate(prompt, InferenceConfigFor(config)));
  ASSIGN_OR_RETURN(std::string projection,
                   CanonicalizeProjectionJson(raw_projection));
  std::vector<std::string> invalidated =
      FindInvalidatedFacts(projection, correction_directives);
  for (int attempt = 0;
       !invalidated.empty() && attempt < config.correction_repair_attempts;
       ++attempt) {
    ASSIGN_OR_RETURN(
        std::string repaired_raw,
        runner_->Generate(CreateCorrectionRepairPrompt(
                              projection, invalidated, correction_directives),
                          InferenceConfigFor(config)));
    ASSIGN_OR_RETURN(projection, CanonicalizeProjectionJson(repaired_raw));
    invalidated = FindInvalidatedFacts(projection, correction_directives);
  }
  RETURN_IF_ERROR(
      ValidateNoInvalidatedFacts(projection, correction_directives));
  return projection;
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

absl::StatusOr<std::string> DPMProjector::CreateProjectionPromptWithCorrections(
    const EventSourcedLog& log, const ProjectionConfig& config,
    const std::vector<ProjectionCorrectionDirective>& correction_directives)
    const {
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
  ASSIGN_OR_RETURN(std::vector<Event> events, log.GetAllEvents());
  ASSIGN_OR_RETURN(
      std::string event_log,
      GetCorrectionAppliedProjectionEventLogRange(
          log, 0, events.size(), correction_directives));
  ASSIGN_OR_RETURN(
      std::string prompt,
      litert::lm::CreateProjectionPrompt(
          event_log, config.schema_id, config.schema_json,
          config.memory_budget_chars, config.max_event_log_chars,
          correction_directives));
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

absl::StatusOr<std::string>
DPMProjector::CreateProjectionPromptForRangeWithCorrections(
    const EventSourcedLog& log, uint64_t event_range_start,
    uint64_t event_range_end, const ProjectionConfig& config,
    const std::vector<ProjectionCorrectionDirective>& correction_directives)
    const {
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
  ASSIGN_OR_RETURN(
      std::string event_log,
      GetCorrectionAppliedProjectionEventLogRange(
          log, event_range_start, event_range_end, correction_directives));
  ASSIGN_OR_RETURN(
      std::string prompt,
      litert::lm::CreateProjectionPrompt(
          event_log, config.schema_id, config.schema_json,
          config.memory_budget_chars, config.max_event_log_chars,
          correction_directives));
  return prompt;
}

}  // namespace litert::lm
