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

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/numbers.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "nlohmann/json.hpp"  // from @nlohmann_json
#include "runtime/dpm/event.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/dpm/projection_prompt.h"
#include "tools/vet/vet_aid.h"
#include "tools/vet/vet_trace.h"

namespace {

using ::litert::lm::DPMLogIdentity;
using ::litert::lm::Event;
using ::litert::lm::EventSourcedLog;
using ::litert::lm::EventToJsonLine;
using ::litert::lm::EventTypeFromString;
using ::litert::lm::ProjectionCorrectionDirective;
using ::litert::lm::ProjectionCorrectionScopeFromString;
using ::litert::lm::ProjectionCorrectionScopeToString;
using ::litert::lm::vet::BuildDefaultAid;
using ::litert::lm::vet::BuildHandoffBundle;
using ::litert::lm::vet::BuildHandoffText;
using ::litert::lm::vet::ComputeAidDigest;
using ::litert::lm::vet::ComputeTraceDigestFromFile;
using ::litert::lm::vet::HandoffBundleRequest;
using ::litert::lm::vet::kDefaultSchemaId;
using ::litert::lm::vet::MakeSessionPaths;
using ::litert::lm::vet::ParseJsonInput;
using ::litert::lm::vet::ReadAidFile;
using ::litert::lm::vet::SessionPaths;
using ::litert::lm::vet::VerifyHandoffBundle;
using ::litert::lm::vet::WriteAidFile;

constexpr absl::string_view kLegacyDefaultSchemaId = kDefaultSchemaId;

std::string Usage() {
  return R"usage(VET: durable memory sidecar for coding agents.

Usage:
  vet init [--root .vet] [--tenant local] [--session NAME]
  vet record --type user|model|tool|internal|correction (--payload TEXT|--stdin)
  vet correction --text TEXT [--invalidated-fact TEXT] [--replacement-fact TEXT]
  vet status [--json]
  vet events [--max-events N]
  vet prompt --task TEXT [--memory-budget-chars N] [--max-events N]
  vet handoff --task TEXT [--format text|json] [--max-events N] [--out FILE]
  vet verify (--bundle FILE | --stdin) [--json]

Common flags:
  --root PATH       Event log root directory. Default: .vet
  --tenant ID       Tenant id. Default: local
  --session ID      Session id. Default: sanitized repository directory name
  --model ID        Optional model id stored on new events
)usage";
}

bool StartsWith(const std::string& text, absl::string_view prefix) {
  return text.rfind(std::string(prefix), 0) == 0;
}

std::string SanitizeIdentity(std::string value, absl::string_view fallback) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                       c == '.';
    out.push_back(valid ? c : '-');
  }
  while (!out.empty() && out.front() == '.') out.erase(out.begin());
  if (out.empty() || out == "." || out == "..") {
    return std::string(fallback);
  }
  return out;
}

std::string DefaultSessionId() {
  std::error_code error;
  std::filesystem::path cwd = std::filesystem::current_path(error);
  if (error) return "default";
  return SanitizeIdentity(cwd.filename().string(), "default");
}

int64_t NowUnixMicros() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

std::string ReadStdin() {
  std::ostringstream buffer;
  buffer << std::cin.rdbuf();
  return buffer.str();
}

absl::StatusOr<std::string> ReadTextFile(const std::string& path) {
  std::ifstream input(path, std::ios::in | std::ios::binary);
  if (!input.is_open()) {
    return absl::InvalidArgumentError(absl::StrCat("cannot read file: ", path));
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

absl::StatusOr<size_t> ParseSize(absl::string_view text,
                                 absl::string_view flag_name) {
  uint64_t value = 0;
  if (!absl::SimpleAtoi(text, &value)) {
    return absl::InvalidArgumentError(
        absl::StrCat(flag_name, " must be a non-negative integer."));
  }
  return static_cast<size_t>(value);
}

bool TakeFlagValue(const std::vector<std::string>& args, size_t* i,
                   absl::string_view flag, std::string* value,
                   absl::Status* error) {
  const std::string flag_string(flag);
  const std::string prefix = absl::StrCat(flag, "=");
  const std::string& arg = args[*i];
  if (arg == flag_string) {
    if (*i + 1 >= args.size()) {
      *error = absl::InvalidArgumentError(
          absl::StrCat("missing value for ", flag));
      return true;
    }
    *value = args[++(*i)];
    return true;
  }
  if (StartsWith(arg, prefix)) {
    *value = arg.substr(prefix.size());
    return true;
  }
  return false;
}

struct CommonOptions {
  std::string root = ".vet";
  std::string tenant = "local";
  std::string session = DefaultSessionId();
  std::string model_id;
};

bool ParseCommonFlag(const std::vector<std::string>& args, size_t* i,
                     CommonOptions* options, absl::Status* error) {
  std::string value;
  if (TakeFlagValue(args, i, "--root", &value, error)) {
    options->root = value;
    return true;
  }
  if (TakeFlagValue(args, i, "--tenant", &value, error)) {
    options->tenant = SanitizeIdentity(value, "local");
    return true;
  }
  if (TakeFlagValue(args, i, "--session", &value, error)) {
    options->session = SanitizeIdentity(value, "default");
    return true;
  }
  if (TakeFlagValue(args, i, "--model", &value, error)) {
    options->model_id = value;
    return true;
  }
  return false;
}

EventSourcedLog OpenLog(const CommonOptions& options) {
  return EventSourcedLog(
      std::filesystem::path(options.root),
      DPMLogIdentity{.tenant_id = options.tenant,
                     .session_id = options.session});
}

std::string RenderEventLog(const std::vector<Event>& events,
                           size_t max_events) {
  const size_t start =
      max_events > 0 && max_events < events.size() ? events.size() - max_events
                                                   : 0;
  std::string out;
  for (size_t i = start; i < events.size(); ++i) {
    if (!out.empty()) out.push_back('\n');
    absl::StrAppend(&out, "[", i + 1, "] ", EventToJsonLine(events[i]));
  }
  return out;
}

std::vector<std::string> JsonStringList(const nlohmann::ordered_json& json,
                                        absl::string_view key) {
  std::vector<std::string> values;
  const auto it = json.find(std::string(key));
  if (it == json.end() || !it->is_array()) return values;
  for (const auto& item : *it) {
    if (item.is_string()) values.push_back(item.get<std::string>());
  }
  return values;
}

std::vector<ProjectionCorrectionDirective> BuildGenericCorrectionDirectives(
    const std::vector<Event>& events) {
  std::vector<ProjectionCorrectionDirective> directives;
  for (size_t i = 0; i < events.size(); ++i) {
    const Event& event = events[i];
    if (event.type != Event::Type::kCorrection) continue;

    ProjectionCorrectionDirective directive;
    directive.correction_event_index = i;
    directive.correction_event_id = absl::StrCat("event-", i + 1);
    directive.correction_text = event.payload;

    try {
      nlohmann::ordered_json payload =
          nlohmann::ordered_json::parse(event.payload);
      if (payload.is_object()) {
        directive.correction_event_id =
            payload.value("correction_id", directive.correction_event_id);
        directive.correction_text =
            payload.value("correction_text", directive.correction_text);
        directive.invalidated_facts =
            JsonStringList(payload, "invalidated_facts");
        directive.replacement_facts =
            JsonStringList(payload, "replacement_facts");
        const std::string scope = payload.value("scope", std::string());
        if (!scope.empty()) {
          auto parsed_scope = ProjectionCorrectionScopeFromString(scope);
          if (parsed_scope.ok()) directive.scope = *parsed_scope;
        }
      }
    } catch (const std::exception&) {
      // Plain-text correction events are still rendered as blocking text.
    }
    directives.push_back(std::move(directive));
  }
  return directives;
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
  if (!wrote_fact) absl::StrAppend(out, "    - none declared\n");
}

std::string FormatVetCorrectionDirectives(
    const std::vector<ProjectionCorrectionDirective>& directives) {
  if (directives.empty()) return "";
  std::string out =
      "[VET BLOCKING CORRECTIONS]\n"
      "Apply every correction below before producing active task memory. "
      "Suppress invalidated facts exactly; prefer replacement facts when "
      "provided; if a conflict remains, emit unknown instead of the old fact. "
      "Invalidated facts listed here are suppression targets, not active "
      "memory.\n";
  for (const ProjectionCorrectionDirective& directive : directives) {
    absl::StrAppend(
        &out, "- correction_id: ",
        directive.correction_event_id.empty() ? "unknown"
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
      "Compliance, decisions, plans, release notes, or handoffs.\n\n");
  return out;
}

std::string DefaultSchemaJson(absl::string_view task) {
  nlohmann::ordered_json schema = nlohmann::ordered_json::object();
  schema["task"] = task.empty() ? "current coding-agent task" : std::string(task);
  schema["mode"] = "coding_agent_sidecar";
  schema["required_sections"] = {
      "current_goal",
      "repo_facts",
      "accepted_decisions",
      "active_constraints",
      "corrections_to_apply",
      "open_questions",
  };
  schema["forgetting_policy"] =
      "Correction events supersede conflicting older facts. Invalidated facts "
      "must not be carried into the active task memory.";
  schema["citation_policy"] =
      "Every claim must cite a one-based event index from the VET log.";
  return schema.dump();
}

absl::Status AppendEvent(const CommonOptions& options, Event::Type type,
                         absl::string_view payload, int64_t step_index = 0,
                         absl::string_view tool_call_id = "") {
  if (payload.empty()) {
    return absl::InvalidArgumentError("event payload must not be empty.");
  }
  EventSourcedLog log = OpenLog(options);
  int64_t resolved_step = step_index;
  if (resolved_step <= 0) {
    auto existing = log.GetAllEvents();
    resolved_step = existing.ok()
                        ? static_cast<int64_t>(existing->size()) + 1
                        : 1;
  }
  std::string resolved_tool_call_id(tool_call_id);
  if (type == Event::Type::kTool && resolved_tool_call_id.empty()) {
    resolved_tool_call_id = absl::StrCat("tool-", resolved_step);
  }
  return log.Append(Event{
      .type = type,
      .tenant_id = options.tenant,
      .session_id = options.session,
      .payload = std::string(payload),
      .timestamp_us = NowUnixMicros(),
      .model_id = options.model_id,
      .step_index = resolved_step,
      .tool_call_id = resolved_tool_call_id,
  });
}

absl::Status RunInit(const std::vector<std::string>& args) {
  CommonOptions options;
  absl::Status error;
  for (size_t i = 2; i < args.size(); ++i) {
    if (ParseCommonFlag(args, &i, &options, &error)) {
      if (!error.ok()) return error;
      continue;
    }
    return absl::InvalidArgumentError(absl::StrCat("unknown flag: ", args[i]));
  }
  const SessionPaths paths =
      MakeSessionPaths(options.root, options.tenant, options.session);
  std::error_code fs_error;
  std::filesystem::create_directories(paths.session_dir, fs_error);
  if (fs_error) {
    return absl::InternalError(
        absl::StrCat("failed to create VET log directory: ",
                     fs_error.message()));
  }
  const nlohmann::ordered_json aid =
      BuildDefaultAid(paths, options.model_id, NowUnixMicros());
  absl::Status aid_status = WriteAidFile(paths, aid);
  if (!aid_status.ok()) return aid_status;
  std::cout << "VET initialized\n"
            << "root: " << options.root << "\n"
            << "tenant: " << options.tenant << "\n"
            << "session: " << options.session << "\n"
            << "path: " << paths.log_path.string() << "\n"
            << "aid: " << paths.aid_path.string() << "\n";
  return absl::OkStatus();
}

absl::Status RunRecord(const std::vector<std::string>& args) {
  CommonOptions options;
  std::string type = "user";
  std::string payload;
  bool read_stdin = false;
  int64_t step_index = 0;
  std::string tool_call_id;
  absl::Status error;

  for (size_t i = 2; i < args.size(); ++i) {
    std::string value;
    if (ParseCommonFlag(args, &i, &options, &error)) {
      if (!error.ok()) return error;
      continue;
    }
    if (TakeFlagValue(args, &i, "--type", &value, &error)) {
      if (!error.ok()) return error;
      type = value;
      continue;
    }
    if (TakeFlagValue(args, &i, "--payload", &value, &error)) {
      if (!error.ok()) return error;
      payload = value;
      continue;
    }
    if (TakeFlagValue(args, &i, "--step-index", &value, &error)) {
      if (!error.ok()) return error;
      uint64_t parsed = 0;
      if (!absl::SimpleAtoi(value, &parsed) || parsed == 0) {
        return absl::InvalidArgumentError(
            "--step-index must be a positive integer.");
      }
      step_index = static_cast<int64_t>(parsed);
      continue;
    }
    if (TakeFlagValue(args, &i, "--tool-call-id", &value, &error)) {
      if (!error.ok()) return error;
      tool_call_id = value;
      continue;
    }
    if (args[i] == "--stdin") {
      read_stdin = true;
      continue;
    }
    return absl::InvalidArgumentError(absl::StrCat("unknown flag: ", args[i]));
  }

  if (read_stdin) payload = ReadStdin();
  auto parsed_type = EventTypeFromString(type);
  if (!parsed_type.ok()) return parsed_type.status();
  absl::Status status =
      AppendEvent(options, *parsed_type, payload, step_index, tool_call_id);
  if (!status.ok()) return status;
  std::cout << "recorded " << type << " event for " << options.tenant << "/"
            << options.session << "\n";
  return absl::OkStatus();
}

absl::Status RunCorrection(const std::vector<std::string>& args) {
  CommonOptions options;
  std::string text;
  std::string scope = "global";
  std::vector<std::string> invalidated_facts;
  std::vector<std::string> replacement_facts;
  absl::Status error;

  for (size_t i = 2; i < args.size(); ++i) {
    std::string value;
    if (ParseCommonFlag(args, &i, &options, &error)) {
      if (!error.ok()) return error;
      continue;
    }
    if (TakeFlagValue(args, &i, "--text", &value, &error)) {
      if (!error.ok()) return error;
      text = value;
      continue;
    }
    if (TakeFlagValue(args, &i, "--scope", &value, &error)) {
      if (!error.ok()) return error;
      if (!ProjectionCorrectionScopeFromString(value).ok()) {
        return absl::InvalidArgumentError(
            "scope must be prior_events, checkpoint_range, or global.");
      }
      scope = value;
      continue;
    }
    if (TakeFlagValue(args, &i, "--invalidated-fact", &value, &error)) {
      if (!error.ok()) return error;
      invalidated_facts.push_back(value);
      continue;
    }
    if (TakeFlagValue(args, &i, "--replacement-fact", &value, &error)) {
      if (!error.ok()) return error;
      replacement_facts.push_back(value);
      continue;
    }
    return absl::InvalidArgumentError(absl::StrCat("unknown flag: ", args[i]));
  }

  if (text.empty() && invalidated_facts.empty() && replacement_facts.empty()) {
    return absl::InvalidArgumentError(
        "correction requires --text, --invalidated-fact, or --replacement-fact.");
  }

  const int64_t created = NowUnixMicros();
  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  payload["kind"] = "vet_correction";
  payload["correction_id"] = absl::StrCat("vet-", created);
  payload["correction_text"] = text;
  payload["invalidated_facts"] = invalidated_facts;
  payload["replacement_facts"] = replacement_facts;
  payload["scope"] = scope;
  payload["created_unix_micros"] = created;

  absl::Status status =
      AppendEvent(options, Event::Type::kCorrection, payload.dump());
  if (!status.ok()) return status;
  std::cout << "recorded correction for " << options.tenant << "/"
            << options.session << "\n";
  return absl::OkStatus();
}

absl::StatusOr<std::vector<Event>> LoadEvents(const CommonOptions& options) {
  EventSourcedLog log = OpenLog(options);
  return log.GetAllEvents();
}

absl::Status RunStatus(const std::vector<std::string>& args) {
  CommonOptions options;
  bool as_json = false;
  absl::Status error;
  for (size_t i = 2; i < args.size(); ++i) {
    if (ParseCommonFlag(args, &i, &options, &error)) {
      if (!error.ok()) return error;
      continue;
    }
    if (args[i] == "--json") {
      as_json = true;
      continue;
    }
    return absl::InvalidArgumentError(absl::StrCat("unknown flag: ", args[i]));
  }

  auto events = LoadEvents(options);
  if (!events.ok()) return events.status();
  size_t user = 0;
  size_t model = 0;
  size_t tool = 0;
  size_t internal = 0;
  size_t correction = 0;
  for (const Event& event : *events) {
    switch (event.type) {
      case Event::Type::kUser:
        ++user;
        break;
      case Event::Type::kModel:
        ++model;
        break;
      case Event::Type::kTool:
        ++tool;
        break;
      case Event::Type::kInternal:
        ++internal;
        break;
      case Event::Type::kCorrection:
        ++correction;
        break;
    }
  }
  EventSourcedLog log = OpenLog(options);
  if (as_json) {
    nlohmann::ordered_json json = nlohmann::ordered_json::object();
    json["root"] = options.root;
    json["tenant"] = options.tenant;
    json["session"] = options.session;
    json["path"] = log.path().string();
    json["events"] = events->size();
    json["user"] = user;
    json["model"] = model;
    json["tool"] = tool;
    json["internal"] = internal;
    json["correction"] = correction;
    std::cout << json.dump() << "\n";
  } else {
    std::cout << "VET status\n"
              << "root: " << options.root << "\n"
              << "tenant: " << options.tenant << "\n"
              << "session: " << options.session << "\n"
              << "path: " << log.path().string() << "\n"
              << "events: " << events->size() << "\n"
              << "user: " << user << "\n"
              << "model: " << model << "\n"
              << "tool: " << tool << "\n"
              << "internal: " << internal << "\n"
              << "correction: " << correction << "\n";
  }
  return absl::OkStatus();
}

absl::Status RunEvents(const std::vector<std::string>& args) {
  CommonOptions options;
  size_t max_events = 0;
  absl::Status error;
  for (size_t i = 2; i < args.size(); ++i) {
    std::string value;
    if (ParseCommonFlag(args, &i, &options, &error)) {
      if (!error.ok()) return error;
      continue;
    }
    if (TakeFlagValue(args, &i, "--max-events", &value, &error)) {
      if (!error.ok()) return error;
      auto parsed = ParseSize(value, "--max-events");
      if (!parsed.ok()) return parsed.status();
      max_events = *parsed;
      continue;
    }
    return absl::InvalidArgumentError(absl::StrCat("unknown flag: ", args[i]));
  }
  auto events = LoadEvents(options);
  if (!events.ok()) return events.status();
  std::cout << RenderEventLog(*events, max_events);
  if (!events->empty()) std::cout << "\n";
  return absl::OkStatus();
}

absl::Status RunPrompt(const std::vector<std::string>& args) {
  CommonOptions options;
  std::string task;
  std::string schema_id(kLegacyDefaultSchemaId);
  std::string schema_json;
  size_t memory_budget_chars = 4096;
  size_t max_event_log_chars = 1 << 20;
  size_t max_events = 0;
  absl::Status error;

  for (size_t i = 2; i < args.size(); ++i) {
    std::string value;
    if (ParseCommonFlag(args, &i, &options, &error)) {
      if (!error.ok()) return error;
      continue;
    }
    if (TakeFlagValue(args, &i, "--task", &value, &error)) {
      if (!error.ok()) return error;
      task = value;
      continue;
    }
    if (TakeFlagValue(args, &i, "--schema-id", &value, &error)) {
      if (!error.ok()) return error;
      schema_id = value;
      continue;
    }
    if (TakeFlagValue(args, &i, "--schema-json", &value, &error)) {
      if (!error.ok()) return error;
      schema_json = value;
      continue;
    }
    if (TakeFlagValue(args, &i, "--schema-file", &value, &error)) {
      if (!error.ok()) return error;
      auto file = ReadTextFile(value);
      if (!file.ok()) return file.status();
      schema_json = *file;
      continue;
    }
    if (TakeFlagValue(args, &i, "--memory-budget-chars", &value, &error)) {
      if (!error.ok()) return error;
      auto parsed = ParseSize(value, "--memory-budget-chars");
      if (!parsed.ok()) return parsed.status();
      memory_budget_chars = *parsed;
      continue;
    }
    if (TakeFlagValue(args, &i, "--max-event-log-chars", &value, &error)) {
      if (!error.ok()) return error;
      auto parsed = ParseSize(value, "--max-event-log-chars");
      if (!parsed.ok()) return parsed.status();
      max_event_log_chars = *parsed;
      continue;
    }
    if (TakeFlagValue(args, &i, "--max-events", &value, &error)) {
      if (!error.ok()) return error;
      auto parsed = ParseSize(value, "--max-events");
      if (!parsed.ok()) return parsed.status();
      max_events = *parsed;
      continue;
    }
    return absl::InvalidArgumentError(absl::StrCat("unknown flag: ", args[i]));
  }

  if (schema_json.empty()) schema_json = DefaultSchemaJson(task);
  auto events = LoadEvents(options);
  if (!events.ok()) return events.status();
  const std::string event_log = RenderEventLog(*events, max_events);
  auto parts = litert::lm::CreateProjectionPromptParts(
      event_log, schema_id, schema_json, memory_budget_chars,
      max_event_log_chars);
  if (!parts.ok()) return parts.status();
  std::cout << parts->cacheable_prefix
            << FormatVetCorrectionDirectives(
                   BuildGenericCorrectionDirectives(*events))
            << parts->event_log_suffix;
  return absl::OkStatus();
}

absl::Status RunHandoff(const std::vector<std::string>& args) {
  CommonOptions options;
  std::string task = "current coding-agent task";
  std::string format = "text";
  std::string out_path;
  size_t max_events = 40;
  absl::Status error;

  for (size_t i = 2; i < args.size(); ++i) {
    std::string value;
    if (ParseCommonFlag(args, &i, &options, &error)) {
      if (!error.ok()) return error;
      continue;
    }
    if (TakeFlagValue(args, &i, "--task", &value, &error)) {
      if (!error.ok()) return error;
      task = value;
      continue;
    }
    if (TakeFlagValue(args, &i, "--format", &value, &error)) {
      if (!error.ok()) return error;
      if (value != "text" && value != "json") {
        return absl::InvalidArgumentError(
            "--format must be text or json.");
      }
      format = value;
      continue;
    }
    if (TakeFlagValue(args, &i, "--out", &value, &error)) {
      if (!error.ok()) return error;
      out_path = value;
      continue;
    }
    if (TakeFlagValue(args, &i, "--max-events", &value, &error)) {
      if (!error.ok()) return error;
      auto parsed = ParseSize(value, "--max-events");
      if (!parsed.ok()) return parsed.status();
      max_events = *parsed;
      continue;
    }
    return absl::InvalidArgumentError(absl::StrCat("unknown flag: ", args[i]));
  }

  const SessionPaths paths =
      MakeSessionPaths(options.root, options.tenant, options.session);
  auto events = LoadEvents(options);
  if (!events.ok()) return events.status();

  auto aid = ReadAidFile(paths);
  if (!aid.ok()) {
    nlohmann::ordered_json created =
        BuildDefaultAid(paths, options.model_id, NowUnixMicros());
    absl::Status write_status = WriteAidFile(paths, created);
    if (!write_status.ok()) return write_status;
    aid = created;
  }

  auto aid_digest = ComputeAidDigest(*aid);
  if (!aid_digest.ok()) return aid_digest.status();
  auto trace_digest = ComputeTraceDigestFromFile(paths.log_path.string());
  if (!trace_digest.ok()) return trace_digest.status();

  const std::vector<ProjectionCorrectionDirective> directives =
      BuildGenericCorrectionDirectives(*events);
  const std::string output_text =
      BuildHandoffText(task, paths, *events, directives, max_events);

  std::string rendered;
  if (format == "json") {
    HandoffBundleRequest request{
        .paths = paths,
        .task = task,
        .output_text = output_text,
        .events = &(*events),
        .corrections = &directives,
        .max_events = max_events,
        .created_unix_micros = NowUnixMicros(),
        .aid = *aid,
        .aid_digest = *aid_digest,
        .trace_digest = *trace_digest,
    };
    rendered = BuildHandoffBundle(request).dump(2);
  } else {
    rendered = output_text;
  }

  if (!out_path.empty()) {
    std::ofstream output(out_path, std::ios::out | std::ios::binary);
    if (!output.is_open()) {
      return absl::InvalidArgumentError(
          absl::StrCat("cannot write handoff output: ", out_path));
    }
    output << rendered;
    if (!output.good()) {
      return absl::InternalError(
          absl::StrCat("failed while writing handoff output: ", out_path));
    }
    std::cout << "wrote " << format << " handoff to " << out_path << "\n";
    return absl::OkStatus();
  }

  std::cout << rendered;
  if (format == "text" && !rendered.empty() && rendered.back() != '\n') {
    std::cout << "\n";
  }
  return absl::OkStatus();
}

absl::Status RunVerify(const std::vector<std::string>& args) {
  CommonOptions options;
  std::string bundle_path;
  bool read_stdin = false;
  bool as_json = false;
  absl::Status error;

  for (size_t i = 2; i < args.size(); ++i) {
    std::string value;
    if (ParseCommonFlag(args, &i, &options, &error)) {
      if (!error.ok()) return error;
      continue;
    }
    if (TakeFlagValue(args, &i, "--bundle", &value, &error)) {
      if (!error.ok()) return error;
      bundle_path = value;
      continue;
    }
    if (args[i] == "--stdin") {
      read_stdin = true;
      continue;
    }
    if (args[i] == "--json") {
      as_json = true;
      continue;
    }
    return absl::InvalidArgumentError(absl::StrCat("unknown flag: ", args[i]));
  }

  if (bundle_path.empty() && !read_stdin) {
    return absl::InvalidArgumentError(
        "verify requires --bundle FILE or --stdin.");
  }

  std::string bundle_text;
  if (read_stdin) {
    bundle_text = ReadStdin();
  } else {
    auto file = ReadTextFile(bundle_path);
    if (!file.ok()) return file.status();
    bundle_text = *file;
  }

  auto bundle = ParseJsonInput(bundle_text);
  if (!bundle.ok()) return bundle.status();

  const SessionPaths paths =
      MakeSessionPaths(options.root, options.tenant, options.session);
  auto report = VerifyHandoffBundle(*bundle, paths);
  if (!report.ok()) return report.status();

  if (as_json) {
    nlohmann::ordered_json json = nlohmann::ordered_json::object();
    json["verified"] = report->verified;
    json["checks_passed"] = report->checks_passed;
    json["checks_failed"] = report->checks_failed;
    json["failure_details"] = report->failure_details;
    json["trace_valid"] = report->trace_report.valid;
    json["trace_errors"] = report->trace_report.errors;
    json["trace_warnings"] = report->trace_report.warnings;
    std::cout << json.dump(2) << "\n";
    return report->verified ? absl::OkStatus()
                            : absl::FailedPreconditionError(
                                  "handoff bundle verification failed.");
  }

  std::cout << (report->verified ? "VERIFIED" : "FAILED") << "\n";
  for (const std::string& check : report->checks_passed) {
    std::cout << "  pass: " << check << "\n";
  }
  for (const std::string& check : report->checks_failed) {
    std::cout << "  fail: " << check;
    const auto detail = report->failure_details.find(check);
    if (detail != report->failure_details.end() && detail->is_string()) {
      std::cout << " — " << detail->get<std::string>();
    }
    std::cout << "\n";
  }
  for (const std::string& error : report->trace_report.errors) {
    std::cout << "  trace: " << error << "\n";
  }
  for (const std::string& warning : report->trace_report.warnings) {
    std::cout << "  warn: " << warning << "\n";
  }
  return report->verified ? absl::OkStatus()
                          : absl::FailedPreconditionError(
                                "handoff bundle verification failed.");
}

absl::Status RunCommand(const std::vector<std::string>& args) {
  if (args.size() < 2 || args[1] == "help" || args[1] == "--help" ||
      args[1] == "-h") {
    std::cout << Usage();
    return absl::OkStatus();
  }
  const std::string& command = args[1];
  if (command == "init") return RunInit(args);
  if (command == "record") return RunRecord(args);
  if (command == "correction") return RunCorrection(args);
  if (command == "status") return RunStatus(args);
  if (command == "events") return RunEvents(args);
  if (command == "prompt") return RunPrompt(args);
  if (command == "handoff") return RunHandoff(args);
  if (command == "verify") return RunVerify(args);
  return absl::InvalidArgumentError(absl::StrCat("unknown command: ", command));
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(argc);
  for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
  absl::Status status = RunCommand(args);
  if (!status.ok()) {
    std::cerr << "vet: " << status.message() << "\n";
    if (status.code() == absl::StatusCode::kInvalidArgument ||
        status.code() == absl::StatusCode::kUnimplemented) {
      std::cerr << "\n" << Usage();
    }
    return 1;
  }
  return 0;
}
