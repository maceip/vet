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

#include "tools/benchmarks/dpm_projection_cliff/scenario/session_case_loader.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "nlohmann/json.hpp"  // from @nlohmann_json

namespace litert::lm::bench {
namespace {

using json = ::nlohmann::json;

// Get a string field if present, else empty. Tolerant of missing
// fields so old golden fixtures don't break when the schema grows.
std::string GetStr(const json& obj, absl::string_view key) {
  auto it = obj.find(std::string(key));
  if (it == obj.end()) return "";
  if (it->is_string()) return it->get<std::string>();
  if (it->is_null()) return "";
  // Coerce numbers / bools to text so we don't crash on schema drift.
  return it->dump();
}

int64_t GetI64(const json& obj, absl::string_view key,
               int64_t fallback = 0) {
  auto it = obj.find(std::string(key));
  if (it == obj.end() || it->is_null()) return fallback;
  if (it->is_number_integer()) return it->get<int64_t>();
  if (it->is_number_unsigned()) return static_cast<int64_t>(it->get<uint64_t>());
  if (it->is_string()) {
    try { return std::stoll(it->get<std::string>()); } catch (...) {}
  }
  return fallback;
}

absl::StatusOr<SessionEvent> ParseEvent(const json& obj) {
  if (!obj.is_object()) {
    return absl::InvalidArgumentError(
        "session event must be a JSON object");
  }
  SessionEvent e;
  e.idx = GetI64(obj, "idx");
  e.kind = GetStr(obj, "kind");
  e.role = GetStr(obj, "role");
  e.text = GetStr(obj, "text");
  e.timestamp = GetStr(obj, "timestamp");
  e.tool_name = GetStr(obj, "tool_name");
  e.tool_args = GetStr(obj, "tool_args");
  e.raw_uuid = GetStr(obj, "raw_uuid");
  return e;
}

// Decode a JSON array of strings into a vector<string>. Tolerant of
// missing field — returns empty vector. Tolerant of non-string entries —
// silently skips them.
std::vector<std::string> GetStrArray(const json& obj,
                                     absl::string_view key) {
  std::vector<std::string> out;
  auto it = obj.find(std::string(key));
  if (it == obj.end() || !it->is_array()) return out;
  out.reserve(it->size());
  for (const auto& el : *it) {
    if (el.is_string()) out.push_back(el.get<std::string>());
  }
  return out;
}

absl::StatusOr<SessionProbe> ParseProbe(const json& obj) {
  if (!obj.is_object()) {
    return absl::InvalidArgumentError("probe must be a JSON object");
  }
  SessionProbe p;
  p.kind = GetStr(obj, "kind");
  p.question = GetStr(obj, "question");
  p.rationale = GetStr(obj, "rationale");
  auto em = obj.find("expected_match");
  if (em != obj.end() && em->is_object()) {
    p.expected_match.substring = GetStr(*em, "substring");
    p.expected_match.tool_name = GetStr(*em, "tool_name");
    p.expected_match.arg_substring = GetStr(*em, "arg_substring");
    p.expected_match.correction_substring =
        GetStr(*em, "correction_substring");
    p.expected_match.must_acknowledge = GetStr(*em, "must_acknowledge");
  }
  // Rubric-shaped ground truth lives at the probe top-level (not
  // nested under expected_match) so a probe can carry both styles.
  p.rubric.must_include = GetStrArray(obj, "must_include");
  p.rubric.must_not_include = GetStrArray(obj, "must_not_include");
  p.rubric.must_call_tools = GetStrArray(obj, "must_call_tools");
  p.rubric.must_not_call_tools = GetStrArray(obj, "must_not_call_tools");
  p.rubric.database_state_must_remain =
      GetStrArray(obj, "database_state_must_remain");
  p.rubric.judge_rubric = GetStr(obj, "judge_rubric");
  return p;
}

absl::StatusOr<SessionCase> ParseCase(const json& obj) {
  if (!obj.is_object()) {
    return absl::InvalidArgumentError(
        "session case must be a JSON object");
  }
  SessionCase c;
  c.case_id = GetStr(obj, "case_id");
  if (c.case_id.empty()) {
    return absl::InvalidArgumentError(
        "session case is missing required field case_id");
  }
  c.domain = GetStr(obj, "domain");
  c.source_path = GetStr(obj, "source_path");
  c.source_sha256 = GetStr(obj, "source_sha256");
  c.n_events = GetI64(obj, "n_events");
  c.probe_T = GetI64(obj, "probe_T");
  c.paired_case_id = GetStr(obj, "paired_case_id");
  c.pair_role = GetStr(obj, "pair_role");

  auto events_it = obj.find("events");
  if (events_it == obj.end() || !events_it->is_array()) {
    return absl::InvalidArgumentError(
        absl::StrCat("session case ", c.case_id,
                     " is missing required array events"));
  }
  c.events.reserve(events_it->size());
  for (const auto& ev : *events_it) {
    auto parsed = ParseEvent(ev);
    if (!parsed.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("session case ", c.case_id, ": ",
                       parsed.status().message()));
    }
    c.events.push_back(std::move(*parsed));
  }

  auto probes_it = obj.find("probes");
  if (probes_it == obj.end() || !probes_it->is_array()) {
    return absl::InvalidArgumentError(
        absl::StrCat("session case ", c.case_id,
                     " is missing required array probes"));
  }
  c.probes.reserve(probes_it->size());
  for (const auto& pr : *probes_it) {
    auto parsed = ParseProbe(pr);
    if (!parsed.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("session case ", c.case_id, ": ",
                       parsed.status().message()));
    }
    c.probes.push_back(std::move(*parsed));
  }
  return c;
}

}  // namespace

absl::StatusOr<std::vector<SessionCase>> ParseSessionCases(
    absl::string_view text) {
  json parsed;
  try {
    parsed = json::parse(text);
  } catch (const json::parse_error& e) {
    return absl::InvalidArgumentError(
        absl::StrCat("session cases JSON parse error: ", e.what()));
  }
  std::vector<SessionCase> out;
  if (parsed.is_array()) {
    out.reserve(parsed.size());
    for (const auto& el : parsed) {
      auto c = ParseCase(el);
      if (!c.ok()) return c.status();
      out.push_back(std::move(*c));
    }
  } else if (parsed.is_object()) {
    auto c = ParseCase(parsed);
    if (!c.ok()) return c.status();
    out.push_back(std::move(*c));
  } else {
    return absl::InvalidArgumentError(
        "session cases JSON must be an object or array");
  }
  return out;
}

absl::StatusOr<std::vector<SessionCase>> LoadSessionCasesFromFile(
    absl::string_view path) {
  std::ifstream in{std::string(path)};
  if (!in.is_open()) {
    return absl::NotFoundError(
        absl::StrCat("LoadSessionCasesFromFile: cannot open ", path));
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  return ParseSessionCases(buf.str());
}

std::string RenderEventLog(const SessionCase& c) {
  // Mirrors the Python ingester's debug rendering: each event becomes
  // one line of "[N] <text>\n" so the substrate's projection input
  // shape is identical between Python-generated synthetic logs and
  // C++-loaded session cases.
  std::ostringstream oss;
  for (const SessionEvent& e : c.events) {
    oss << "[" << (e.idx + 1) << "] " << e.text << "\n";
  }
  return oss.str();
}

}  // namespace litert::lm::bench
