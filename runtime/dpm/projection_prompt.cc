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

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/str_join.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl

namespace litert::lm {
namespace {

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

constexpr absl::string_view kPrefixBoundaryMarker =
    "[DPM PROJECTION PREFIX BOUNDARY v1]\n";

std::string LowerAscii(absl::string_view text) {
  std::string out(text);
  for (char& ch : out) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return out;
}

void AppendFactList(absl::string_view label,
                    const std::vector<std::string>& facts,
                    std::string* out) {
  absl::StrAppend(out, "  ", label, ":\n");
  bool wrote_fact = false;
  for (const std::string& fact : facts) {
    if (fact.empty()) continue;
    absl::StrAppend(out, "    - ", fact, "\n");
    wrote_fact = true;
  }
  if (!wrote_fact) {
    absl::StrAppend(out, "    - none declared\n");
  }
}

}  // namespace

absl::string_view ProjectionCorrectionScopeToString(
    ProjectionCorrectionScope scope) {
  switch (scope) {
    case ProjectionCorrectionScope::kPriorEvents:
      return "prior_events";
    case ProjectionCorrectionScope::kCheckpointRange:
      return "checkpoint_range";
    case ProjectionCorrectionScope::kGlobal:
      return "global";
  }
  return "checkpoint_range";
}

absl::StatusOr<ProjectionCorrectionScope> ProjectionCorrectionScopeFromString(
    absl::string_view scope) {
  if (scope == "prior_events") return ProjectionCorrectionScope::kPriorEvents;
  if (scope == "checkpoint_range") {
    return ProjectionCorrectionScope::kCheckpointRange;
  }
  if (scope == "global") return ProjectionCorrectionScope::kGlobal;
  return absl::InvalidArgumentError(
      absl::StrCat("unknown projection correction scope: ", scope));
}

std::string FormatProjectionCorrectionDirectives(
    const std::vector<ProjectionCorrectionDirective>& directives) {
  if (directives.empty()) return "";

  std::string out =
      "[BLOCKING CORRECTIONS]\n"
      "The checkpoint gate refused stale memory. Apply every correction below "
      "before producing the projection. Suppress invalidated facts exactly; "
      "prefer replacement facts when provided; if a conflict remains, emit "
      "unknown instead of the old fact.\n";
  for (const ProjectionCorrectionDirective& directive : directives) {
    absl::StrAppend(
        &out, "- correction_id: ", directive.correction_event_id.empty()
                                         ? "unknown"
                                         : directive.correction_event_id,
        "\n",
        "  correction_event: [", directive.correction_event_index + 1, "]\n",
        "  scope: ", ProjectionCorrectionScopeToString(directive.scope), "\n");
    if (!directive.correction_text.empty()) {
      absl::StrAppend(&out, "  correction_text: ",
                      directive.correction_text, "\n");
    }
    AppendFactList("invalidated_facts", directive.invalidated_facts, &out);
    AppendFactList("replacement_facts", directive.replacement_facts, &out);
  }
  absl::StrAppend(
      &out,
      "Rules: do not include invalidated facts in Facts, Reasoning, "
      "Compliance, or any decider-facing memory. Keep one-based event "
      "citations for replacement facts.\n\n");
  return out;
}

std::vector<std::string> FindInvalidatedFacts(
    absl::string_view text,
    const std::vector<ProjectionCorrectionDirective>& directives) {
  std::vector<std::string> hits;
  const std::string lowered_text = LowerAscii(text);
  for (const ProjectionCorrectionDirective& directive : directives) {
    for (const std::string& fact : directive.invalidated_facts) {
      if (fact.empty()) continue;
      if (lowered_text.find(LowerAscii(fact)) != std::string::npos &&
          std::find(hits.begin(), hits.end(), fact) == hits.end()) {
        hits.push_back(fact);
      }
    }
  }
  return hits;
}

absl::Status ValidateNoInvalidatedFacts(
    absl::string_view text,
    const std::vector<ProjectionCorrectionDirective>& directives) {
  const std::vector<std::string> hits =
      FindInvalidatedFacts(text, directives);
  if (hits.empty()) return absl::OkStatus();
  return absl::FailedPreconditionError(absl::StrCat(
      "correction-aware projection contains invalidated fact(s): ",
      absl::StrJoin(hits, "; ")));
}

absl::StatusOr<ProjectionPromptParts> CreateProjectionPromptParts(
    absl::string_view event_log, absl::string_view schema_id,
    absl::string_view schema_json, size_t memory_budget_chars,
    size_t max_event_log_chars) {
  return CreateProjectionPromptParts(event_log, schema_id, schema_json,
                                     memory_budget_chars, max_event_log_chars,
                                     {});
}

absl::StatusOr<ProjectionPromptParts> CreateProjectionPromptParts(
    absl::string_view event_log, absl::string_view schema_id,
    absl::string_view schema_json, size_t memory_budget_chars,
    size_t max_event_log_chars,
    const std::vector<ProjectionCorrectionDirective>& correction_directives) {
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
  if (max_event_log_chars > 0 && event_log.size() > max_event_log_chars) {
    return absl::ResourceExhaustedError(absl::StrCat(
        "DPM event log is too large for a single projection prompt (",
        event_log.size(), " bytes > ", max_event_log_chars,
        "); hierarchical projection is required."));
  }
  ProjectionPromptParts parts;
  parts.cacheable_prefix = absl::StrCat(
      kStaticPreamble,
      "[SCHEMA ID]\n", schema_id, "\n\n",
      "[TASK SCHEMA]\n", schema_json, "\n\n",
      "[MEMORY BUDGET]\n", memory_budget_chars, " characters\n\n",
      kPrefixBoundaryMarker);
  // event_log_suffix carries the variable portion: the event log itself
  // plus the JSON-shape contract that pins projection bytes across
  // replays. The contract sits in the suffix (not the cached prefix) so
  // it stays adjacent to the event log when operators grep captured
  // prompts; the determinism harness on phase1 depends on this shape.
  const std::string correction_block =
      FormatProjectionCorrectionDirectives(correction_directives);
  parts.event_log_suffix = absl::StrCat(
      correction_block,
      "[EVENT LOG]\n", event_log, "\n\n",
      "[EXPECTED OUTPUT]\n",
      "Return only a valid JSON object in this exact shape:\n",
      "{\"Facts\":[\"... [i]\"],\"Reasoning\":[\"... [i]\"],"
      "\"Compliance\":[\"... [i]\"]}\n",
      "Do not wrap the JSON in markdown or code fences. The first output "
      "byte must be '{' and the last output byte must be '}'.\n");
  return parts;
}

absl::StatusOr<std::string> CreateProjectionPrompt(
    absl::string_view event_log, absl::string_view schema_id,
    absl::string_view schema_json, size_t memory_budget_chars,
    size_t max_event_log_chars) {
  return CreateProjectionPrompt(event_log, schema_id, schema_json,
                                memory_budget_chars, max_event_log_chars, {});
}

absl::StatusOr<std::string> CreateProjectionPrompt(
    absl::string_view event_log, absl::string_view schema_id,
    absl::string_view schema_json, size_t memory_budget_chars,
    size_t max_event_log_chars,
    const std::vector<ProjectionCorrectionDirective>& correction_directives) {
  auto parts = CreateProjectionPromptParts(event_log, schema_id, schema_json,
                                           memory_budget_chars,
                                           max_event_log_chars,
                                           correction_directives);
  if (!parts.ok()) return parts.status();
  return parts->Compose();
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
