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

#include "runtime/dpm/event.h"

#include <cstdint>
#include <exception>
#include <string>

#include "absl/log/absl_log.h"  // from @com_google_absl
#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "nlohmann/json.hpp"  // from @nlohmann_json

namespace litert::lm {
namespace {

nlohmann::ordered_json EventToJson(const Event& event) {
  nlohmann::ordered_json json = nlohmann::ordered_json::object();
  json["type"] = std::string(EventTypeToString(event.type));
  json["tenant_id"] = event.tenant_id;
  json["session_id"] = event.session_id;
  json["payload"] = event.payload;
  json["timestamp_us"] = event.timestamp_us;
  if (!event.model_id.empty()) {
    json["model_id"] = event.model_id;
  }
  if (event.step_index > 0) {
    json["step_index"] = event.step_index;
  }
  if (!event.tool_call_id.empty()) {
    json["tool_call_id"] = event.tool_call_id;
  }
  return json;
}

absl::StatusOr<Event> EventFromJson(const nlohmann::ordered_json& json) {
  try {
    if (!json.is_object()) {
      return absl::InvalidArgumentError("DPM event JSON must be an object.");
    }
    if (!json.contains("type") || !json["type"].is_string()) {
      return absl::InvalidArgumentError(
          "DPM event JSON is missing string field 'type'.");
    }
    if (!json.contains("payload") || !json["payload"].is_string()) {
      return absl::InvalidArgumentError(
          "DPM event JSON is missing string field 'payload'.");
    }
    if (!json.contains("tenant_id") || !json["tenant_id"].is_string()) {
      return absl::InvalidArgumentError(
          "DPM event JSON is missing string field 'tenant_id'.");
    }
    if (!json.contains("session_id") || !json["session_id"].is_string()) {
      return absl::InvalidArgumentError(
          "DPM event JSON is missing string field 'session_id'.");
    }
    if (!json.contains("timestamp_us") || !json["timestamp_us"].is_number()) {
      return absl::InvalidArgumentError(
          "DPM event JSON is missing numeric field 'timestamp_us'.");
    }

    absl::StatusOr<Event::Type> type =
        EventTypeFromString(json["type"].get<std::string>());
    if (!type.ok()) {
      return type.status();
    }
    return Event{
        .type = *type,
        .tenant_id = json["tenant_id"].get<std::string>(),
        .session_id = json["session_id"].get<std::string>(),
        .payload = json["payload"].get<std::string>(),
        .timestamp_us = json["timestamp_us"].get<int64_t>(),
        .model_id = json.value("model_id", std::string()),
        .step_index = json.value("step_index", static_cast<int64_t>(0)),
        .tool_call_id = json.value("tool_call_id", std::string()),
    };
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid DPM event JSON: ", e.what()));
  }
}

}  // namespace

absl::string_view EventTypeToString(Event::Type type) {
  switch (type) {
    case Event::Type::kUser:
      return "user";
    case Event::Type::kModel:
      return "model";
    case Event::Type::kTool:
      return "tool";
    case Event::Type::kInternal:
      return "internal";
    case Event::Type::kCorrection:
      return "correction";
  }
  ABSL_LOG(FATAL) << "Unknown DPM event type.";
  return "unknown";
}

absl::StatusOr<Event::Type> EventTypeFromString(absl::string_view type) {
  if (type == "user") {
    return Event::Type::kUser;
  }
  if (type == "model") {
    return Event::Type::kModel;
  }
  if (type == "tool") {
    return Event::Type::kTool;
  }
  if (type == "internal") {
    return Event::Type::kInternal;
  }
  if (type == "correction") {
    return Event::Type::kCorrection;
  }
  return absl::InvalidArgumentError(
      absl::StrCat("Unknown DPM event type: ", type));
}

absl::StatusOr<Event> EventFromJsonLine(absl::string_view line) {
  try {
    return EventFromJson(nlohmann::ordered_json::parse(std::string(line)));
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid DPM event JSON line: ", e.what()));
  }
}

std::string EventToJsonLine(const Event& event) {
  return EventToJson(event).dump();
}

}  // namespace litert::lm
