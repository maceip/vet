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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_PROJECTION_COMPARATOR_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_PROJECTION_COMPARATOR_H_

#include <string>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl

namespace litert::lm {

struct ProjectionComparisonConfig {
  std::vector<std::string> required_fields = {
      "Facts",
      "Reasoning",
      "Compliance",
  };
};

struct ProjectionComparisonResult {
  bool matches = false;
  double drift_score = 1.0;
  std::vector<std::string> drift_fields;
};

// Deterministic schema-aware comparator for projected-memory JSON. The score is
// changed_or_missing_required_fields / required_fields, so it stays in [0, 1]
// and provides a useful gradient for audit gates that opt out of byte-exact
// replay. Parse failures are treated as full drift.
absl::StatusOr<ProjectionComparisonResult> CompareStructuredProjectionJson(
    absl::string_view expected_projection,
    absl::string_view observed_projection,
    ProjectionComparisonConfig config = {});

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_PROJECTION_COMPARATOR_H_
