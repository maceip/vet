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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_EVENT_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_EVENT_H_

#include <cstdint>
#include <string>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl

namespace litert::lm {

// Append-only event used as the durable source of truth for DPM memory.
struct Event {
  enum class Type {
    kUser,
    kModel,
    kTool,
    kInternal,
    kCorrection,
  };

  Type type = Type::kUser;
  std::string tenant_id;
  std::string session_id;
  std::string payload;
  int64_t timestamp_us = 0;
  std::string model_id;
  // step_index: one-based position in the session log (0 means unset).
  int64_t step_index = 0;
  // tool_call_id: links a tool event to a specific tool invocation when set.
  std::string tool_call_id;
};

absl::string_view EventTypeToString(Event::Type type);
absl::StatusOr<Event::Type> EventTypeFromString(absl::string_view type);

absl::StatusOr<Event> EventFromJsonLine(absl::string_view line);

std::string EventToJsonLine(const Event& event);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_EVENT_H_
