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

// Returns the static head of a DPM projection prompt for a given
// (schema_id, schema_json, memory_budget_chars) tuple. The output is
// byte-deterministic for those inputs and contains no event-log data, so a
// serving backend can render it once, tokenize it, and pin the resulting KV
// prefix in its cache; subsequent projection calls that share the same head
// only need to prefill the variable tail (the event log).
//
// The returned prefix ends at the boundary between the static header and the
// "[EVENT LOG]" section. CreateProjectionPrompt(prefix + tail) below is
// guaranteed to share this byte-exact prefix for matching configs.
absl::StatusOr<std::string> CreateProjectionPromptPrefix(
    absl::string_view schema_id, absl::string_view schema_json,
    size_t memory_budget_chars);

// Returns the variable tail (the framed event-log section). Concatenating
// CreateProjectionPromptPrefix(...) + CreateProjectionPromptTail(event_log)
// produces the same bytes as CreateProjectionPrompt(event_log, ...).
absl::StatusOr<std::string> CreateProjectionPromptTail(
    absl::string_view event_log, size_t max_event_log_chars);

absl::StatusOr<std::string> CreateProjectionPrompt(
    absl::string_view event_log, absl::string_view schema_id,
    absl::string_view schema_json, size_t memory_budget_chars,
    size_t max_event_log_chars);

std::string CreateDeciderPrompt(absl::string_view projected_memory,
                                absl::string_view case_id,
                                absl::string_view decision_options);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_PROJECTION_PROMPT_H_
