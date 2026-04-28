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

#ifndef THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_SCENARIO_SESSION_CASE_LOADER_H_
#define THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_SCENARIO_SESSION_CASE_LOADER_H_

// Loads SessionCase JSON files emitted by
// tools/benchmarks/dpm_projection_cliff/scenario/ingest_session.py
// into typed C++ structs. The Python ingester turns a single Claude
// or Codex session log into N SessionCase records, one per probe-
// point. Each record carries:
//   - the events from session-start up to (but not including) probe_T
//   - one or more Probes the substrate test will score against
//
// Schema (matches ingest_session.py output):
//
//   [
//     {
//       "case_id": "claude-<uuid>@T=<int>",
//       "domain": "claude" | "codex",
//       "source_path": "...",
//       "source_sha256": "<hex>",
//       "n_events": <int>,
//       "probe_T": <int>,
//       "events": [
//         {"idx": <int>, "kind": "<str>", "role": "<str>",
//          "text": "<str>", "timestamp": "<iso8601>",
//          "tool_name": "<str>", "tool_args": "<str>",
//          "raw_uuid": "<str>"}, ...
//       ],
//       "probes": [
//         {"kind": "next_user_intent" | "next_tool_call" |
//                  "correction_detection",
//          "question": "<str>",
//          "expected_match": { ... varies by kind ... },
//          "rationale": "<str>"}, ...
//       ]
//     }, ...
//   ]
//
// Top-level value is a JSON array (one file -> many SessionCases).
// Single-case JSON objects are also accepted for compatibility with
// hand-curated golden fixtures.

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl

namespace litert::lm::bench {

struct SessionEvent {
  int64_t idx = 0;
  std::string kind;        // "user" | "assistant_text" | "assistant_thinking"
                           // | "tool_call" | "tool_result" | "system"
  std::string role;        // "user" | "assistant" | "system" | "tool"
  std::string text;        // already redacted by the ingester
  std::string timestamp;   // ISO8601
  std::string tool_name;   // set when kind == tool_call/tool_result
  std::string tool_args;   // short repr of arguments
  std::string raw_uuid;    // session uuid for tracing
};

// expected_match has a different shape per probe kind. We carry it as
// the raw decoded fields a property test will check against, rather
// than a generic "json blob" — keeps the test code straightforward.
struct ProbeExpectedMatch {
  // next_user_intent / correction_detection
  std::string substring;            // a substring that must appear in the
                                    // agent's projected memory or response
  // next_tool_call
  std::string tool_name;
  std::string arg_substring;
  // correction_detection
  std::string correction_substring;
  std::string must_acknowledge;     // e.g. "correction"
};

// Rubric-shaped ground truth. Populated by adapters whose source
// dataset has machine-checkable rubrics (AgenticQwen-Data is the
// canonical example). When any of these vectors is non-empty, the
// scenario test must check it in addition to (or instead of) the
// substring-style expected_match. Optional / absent for free-form
// session captures.
struct ProbeRubric {
  // Facts/strings the projected memory MUST surface for the agent to
  // make the right next decision.
  std::vector<std::string> must_include;
  // Facts/strings the projected memory MUST NOT surface (e.g. user-
  // pressure tactics that should not be propagated as authoritative
  // facts).
  std::vector<std::string> must_not_include;
  // Tools the agent MUST invoke next, in the recorded order.
  std::vector<std::string> must_call_tools;
  // Tools the agent MUST NOT invoke (e.g. policy-violating ones).
  std::vector<std::string> must_not_call_tools;
  // Database / external-state predicates that must remain unchanged.
  std::vector<std::string> database_state_must_remain;
  // Optional free-form rubric for a downstream LLM judge to score.
  std::string judge_rubric;
};

struct SessionProbe {
  std::string kind;                 // "next_user_intent" | "next_tool_call"
                                    // | "correction_detection"
                                    // | "policy_preserved"
  std::string question;
  ProbeExpectedMatch expected_match;
  ProbeRubric rubric;               // empty when the source isn't rubric-shaped
  std::string rationale;
};

struct SessionCase {
  std::string case_id;
  std::string domain;               // "claude" | "codex" | "agentic_qwen"
  std::string source_path;
  std::string source_sha256;
  int64_t n_events = 0;
  int64_t probe_T = 0;
  std::vector<SessionEvent> events;
  std::vector<SessionProbe> probes;
  // Twin-pair linkage for adversarial differential tests. When this
  // case has a hack_path counterpart, paired_case_id holds the other
  // case's id; pair_role distinguishes "normal" vs "hack". Empty
  // for un-paired cases.
  std::string paired_case_id;
  std::string pair_role;            // "normal" | "hack" | ""
};

// Load one or more SessionCases from a JSON file.
absl::StatusOr<std::vector<SessionCase>> LoadSessionCasesFromFile(
    absl::string_view path);

// Parse SessionCases from a JSON document already in memory. The string
// must contain either a single SessionCase object or a JSON array of
// SessionCase objects.
absl::StatusOr<std::vector<SessionCase>> ParseSessionCases(
    absl::string_view json);

// Renders the events from index 0 to probe_T into the same line format
// the Python ingester used: "[idx+1] {json-ish}\n". This is the
// shape the substrate's CreateProjectionPromptParts expects on the
// event_log_suffix side. Convenience for scenario tests.
std::string RenderEventLog(const SessionCase& c);

}  // namespace litert::lm::bench

#endif  // THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_SCENARIO_SESSION_CASE_LOADER_H_
