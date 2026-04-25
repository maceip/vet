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

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/util/status_macros.h"

namespace litert::lm {
namespace {

// Static head of the projection prompt. Everything before [EVENT LOG]. This
// is the prefix-cache target: identical bytes for matching
// (schema_id, schema_json, memory_budget_chars) tuples across calls.
constexpr absl::string_view kStaticPreamble =
    "System. You are producing a decision-ready memory view over an event "
    "log for task T. Preserve every dollar amount, date, identifier, and "
    "policy limit verbatim. Cite the event index for each claim. Do not "
    "paraphrase numeric anchors. Output three sections in fixed order.\n\n"
    "Instructions:\n"
    "1. Facts (F): Extract specific anchors. If a required field is not "
    "derivable from the log, emit unknown.\n"
    "2. Reasoning (R): State the inferential steps taken so far based only "
    "on the log.\n"
    "3. Compliance (C): Note any regulatory provisions cited in the events.\n"
    "Constraints: Output MUST be valid JSON with fields Facts, Reasoning, "
    "and Compliance. Temperature 0.0. Reference every fact by its "
    "one-based Event Index [i] in the log. Treat correction events as "
    "superseding earlier conflicting facts.\n\n";

// Marker that downstream tooling can grep for to verify boundary placement
// in captured prompts. Intentionally part of the static head so it lives
// inside the cached prefix.
constexpr absl::string_view kPrefixBoundaryMarker =
    "[DPM PROJECTION PREFIX BOUNDARY v1]\n";

}  // namespace

absl::StatusOr<std::string> CreateProjectionPromptPrefix(
    absl::string_view schema_id, absl::string_view schema_json,
    size_t memory_budget_chars) {
  if (schema_id.empty()) {
    return absl::InvalidArgumentError(
        "DPM projection requires a non-empty schema_id.");
  }
  if (schema_json.empty()) {
    return absl::InvalidArgumentError(
        "DPM projection requires a non-empty task schema.");
  }
  if (memory_budget_chars == 0) {
    return absl::InvalidArgumentError(
        "DPM projection requires a non-zero memory budget.");
  }
  return absl::StrCat(kStaticPreamble,
                      "[SCHEMA ID]\n", schema_id, "\n\n",
                      "[TASK SCHEMA]\n", schema_json, "\n\n",
                      "[MEMORY BUDGET]\n", memory_budget_chars,
                      " characters\n\n",
                      kPrefixBoundaryMarker);
}

absl::StatusOr<std::string> CreateProjectionPromptTail(
    absl::string_view event_log, size_t max_event_log_chars) {
  if (max_event_log_chars > 0 && event_log.size() > max_event_log_chars) {
    return absl::ResourceExhaustedError(absl::StrCat(
        "DPM event log is too large for a single projection prompt (",
        event_log.size(), " bytes > ", max_event_log_chars,
        "); hierarchical projection is required."));
  }
  // Tail = event log + the JSON-shape contract. The contract is part of
  // the tail (not the cached prefix) because keeping it adjacent to the
  // event log preserves the prompt structure operators expect when
  // grepping captured prompts. Pinning the JSON shape here is what makes
  // the projection bytes stable across replays.
  return absl::StrCat(
      "[EVENT LOG]\n", event_log, "\n\n",
      "[EXPECTED OUTPUT]\n",
      "Return only a valid JSON object in this exact shape:\n",
      "{\"Facts\":[\"... [i]\"],\"Reasoning\":[\"... [i]\"],"
      "\"Compliance\":[\"... [i]\"]}\n",
      "Do not wrap the JSON in markdown or code fences. The first output "
      "byte must be '{' and the last output byte must be '}'.\n");
}

absl::StatusOr<std::string> CreateProjectionPrompt(
    absl::string_view event_log, absl::string_view schema_id,
    absl::string_view schema_json, size_t memory_budget_chars,
    size_t max_event_log_chars) {
  ASSIGN_OR_RETURN(std::string prefix,
                   CreateProjectionPromptPrefix(schema_id, schema_json,
                                                memory_budget_chars));
  ASSIGN_OR_RETURN(std::string tail,
                   CreateProjectionPromptTail(event_log, max_event_log_chars));
  return absl::StrCat(prefix, tail);
}

std::string CreateDeciderPrompt(absl::string_view projected_memory,
                                absl::string_view case_id,
                                absl::string_view decision_options) {
  return absl::StrCat(
      "Given the following Projected Memory M, provide a final verdict for "
      "Case ID ",
      case_id, ".\n",
      "Decision options: ", decision_options, ".\n",
      "Base the decision strictly on Facts and Compliance.\n\n",
      "[PROJECTED MEMORY]\n", projected_memory, "\n");
}

}  // namespace litert::lm
