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

#ifndef THIRD_PARTY_ODML_LITERT_LM_TOOLS_VET_VET_TRACE_H_
#define THIRD_PARTY_ODML_LITERT_LM_TOOLS_VET_VET_TRACE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "nlohmann/json.hpp"  // from @nlohmann_json
#include "runtime/dpm/event.h"
#include "runtime/dpm/projection_prompt.h"
#include "tools/vet/vet_aid.h"

namespace litert::lm::vet {

struct ValidTraceReport {
  bool valid = false;
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
};

struct HandoffBundleRequest {
  SessionPaths paths;
  absl::string_view task;
  absl::string_view output_text;
  const std::vector<Event>* events = nullptr;
  const std::vector<ProjectionCorrectionDirective>* corrections = nullptr;
  size_t max_events = 40;
  int64_t created_unix_micros = 0;
  nlohmann::ordered_json aid;
  std::string aid_digest;
  std::string trace_digest;
};

struct VerifyReport {
  bool verified = false;
  std::vector<std::string> checks_passed;
  std::vector<std::string> checks_failed;
  // Maps failed check id -> human-readable reason (parallel to checks_failed).
  nlohmann::ordered_json failure_details = nlohmann::ordered_json::object();
  ValidTraceReport trace_report;
};

// Checks that events belong to the session and follow valid step order.
ValidTraceReport ValidateExecutionTrace(
    const std::vector<Event>& events, const SessionPaths& paths);

absl::StatusOr<std::string> ComputeTraceDigestFromFile(
    absl::string_view log_path);

// Assigns step_index (one-based event ordinal) when unset.
void EnsureTraceCoordinates(std::vector<Event>* events);

nlohmann::ordered_json BuildHandoffBundle(const HandoffBundleRequest& request);

absl::StatusOr<VerifyReport> VerifyHandoffBundle(
    const nlohmann::ordered_json& bundle, const SessionPaths& paths);

absl::StatusOr<nlohmann::ordered_json> ParseJsonInput(
    absl::string_view json_text);

std::string BuildHandoffText(
    absl::string_view task, const SessionPaths& paths,
    const std::vector<Event>& events,
    const std::vector<ProjectionCorrectionDirective>& directives,
    size_t max_events);

}  // namespace litert::lm::vet

#endif  // THIRD_PARTY_ODML_LITERT_LM_TOOLS_VET_VET_TRACE_H_
