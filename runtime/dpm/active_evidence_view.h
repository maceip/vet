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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_ACTIVE_EVIDENCE_VIEW_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_ACTIVE_EVIDENCE_VIEW_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/dpm/projection_prompt.h"

namespace litert::lm {

struct RevokedEvidenceRecord {
  uint64_t global_event_index = 0;
  std::vector<std::string> correction_event_ids;
  std::vector<std::string> invalidated_facts;
  std::string original_event_json;
};

struct ActiveEvidenceView {
  uint64_t event_range_start = 0;
  uint64_t event_range_end = 0;
  std::string active_event_log;
  std::string revoked_evidence_log;
  std::vector<RevokedEvidenceRecord> revoked_records;
};

// Renders a correction-applied view over the immutable event log. Active
// evidence preserves the original global event ids, but events containing
// invalidated facts are replaced with REVOKED_BY_CORRECTION markers before the
// projector sees them. The original revoked bytes remain available through
// revoked_evidence_log for audit/debug paths, not decision prompts.
absl::StatusOr<ActiveEvidenceView> BuildActiveEvidenceView(
    const EventSourcedLog& log, uint64_t event_range_start,
    uint64_t event_range_end,
    const std::vector<ProjectionCorrectionDirective>& correction_directives);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_ACTIVE_EVIDENCE_VIEW_H_
