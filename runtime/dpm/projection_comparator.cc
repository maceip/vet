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

#include "runtime/dpm/projection_comparator.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "nlohmann/json.hpp"

namespace litert::lm {

absl::StatusOr<ProjectionComparisonResult> CompareStructuredProjectionJson(
    absl::string_view expected_projection,
    absl::string_view observed_projection,
    ProjectionComparisonConfig config) {
  if (config.required_fields.empty()) {
    return absl::InvalidArgumentError(
        "projection comparator requires at least one field.");
  }

  nlohmann::json expected;
  nlohmann::json observed;
  try {
    expected = nlohmann::json::parse(expected_projection);
    observed = nlohmann::json::parse(observed_projection);
  } catch (const nlohmann::json::exception& e) {
    ProjectionComparisonResult result;
    result.matches = false;
    result.drift_score = 1.0;
    result.drift_fields = {absl::StrCat("parse_error:", e.what())};
    return result;
  }

  ProjectionComparisonResult result;
  for (const std::string& field : config.required_fields) {
    if (!expected.contains(field) || !observed.contains(field)) {
      result.drift_fields.push_back(field);
      continue;
    }
    if (expected.at(field) != observed.at(field)) {
      result.drift_fields.push_back(field);
    }
  }
  result.matches = result.drift_fields.empty();
  result.drift_score =
      static_cast<double>(result.drift_fields.size()) /
      static_cast<double>(config.required_fields.size());
  return result;
}

}  // namespace litert::lm
