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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_PROJECTION_PROMPT_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_PROJECTION_PROMPT_H_

#include <cstddef>
#include <string>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl

namespace litert::lm {

// Two-part rendering of a DPM projection prompt:
//
//   - cacheable_prefix: byte-deterministic for a given
//     (schema_id, schema_json, memory_budget_chars) tuple. Contains the
//     system instructions, [SCHEMA ID], [TASK SCHEMA], and [MEMORY BUDGET]
//     banners through the prefix-cache boundary marker. A serving backend
//     can render this once and pin the resulting KV prefix; subsequent
//     projection calls that share the same head only prefill the suffix.
//
//   - event_log_suffix: the variable [EVENT LOG] section.
//
// Compose() concatenates the two and is byte-equivalent to the legacy
// CreateProjectionPrompt below. Tests pin both the prefix-stability
// property (same config, different log → identical prefix) and the
// composition equivalence.
struct ProjectionPromptParts {
  std::string cacheable_prefix;
  std::string event_log_suffix;

  std::string Compose() const {
    return cacheable_prefix + event_log_suffix;
  }
};

// Builds the cacheable prefix and the event-log suffix as separate strings.
// Returns InvalidArgument if any of the static-head inputs are empty or
// zero, or ResourceExhausted if event_log exceeds max_event_log_chars.
absl::StatusOr<ProjectionPromptParts> CreateProjectionPromptParts(
    absl::string_view event_log, absl::string_view schema_id,
    absl::string_view schema_json, size_t memory_budget_chars,
    size_t max_event_log_chars);

// Convenience: returns Compose() of the parts. Equivalent in bytes to the
// pre-merge implementation.
absl::StatusOr<std::string> CreateProjectionPrompt(
    absl::string_view event_log, absl::string_view schema_id,
    absl::string_view schema_json, size_t memory_budget_chars,
    size_t max_event_log_chars);

std::string CreateDeciderPrompt(absl::string_view projected_memory,
                                absl::string_view case_id,
                                absl::string_view decision_options);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_PROJECTION_PROMPT_H_
