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

#include "runtime/dpm/active_evidence_view.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/str_join.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/dpm/event.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/dpm/projection_prompt.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {
namespace {

struct PreparedInvalidatedFact {
  std::string original;
  std::string lowered;
};

struct PreparedCorrectionDirective {
  std::string correction_event_id;
  uint64_t correction_event_index = 0;
  ProjectionCorrectionScope scope = ProjectionCorrectionScope::kCheckpointRange;
  std::vector<PreparedInvalidatedFact> invalidated_facts;
};

struct MatchedRevocation {
  std::vector<std::string> correction_event_ids;
  std::vector<std::string> invalidated_facts;
};

std::string LowerAscii(absl::string_view text) {
  std::string out(text);
  for (char& ch : out) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return out;
}

bool CorrectionAppliesToEvent(uint64_t event_index,
                              const PreparedCorrectionDirective& directive) {
  switch (directive.scope) {
    case ProjectionCorrectionScope::kPriorEvents:
      return event_index < directive.correction_event_index;
    case ProjectionCorrectionScope::kCheckpointRange:
    case ProjectionCorrectionScope::kGlobal:
      return true;
  }
  return true;
}

std::vector<PreparedCorrectionDirective> PrepareCorrectionDirectives(
    const std::vector<ProjectionCorrectionDirective>& directives) {
  std::vector<PreparedCorrectionDirective> prepared;
  prepared.reserve(directives.size());
  for (const ProjectionCorrectionDirective& directive : directives) {
    PreparedCorrectionDirective out{
        .correction_event_id = directive.correction_event_id.empty()
                                   ? "unknown"
                                   : directive.correction_event_id,
        .correction_event_index = directive.correction_event_index,
        .scope = directive.scope,
    };
    out.invalidated_facts.reserve(directive.invalidated_facts.size());
    for (const std::string& fact : directive.invalidated_facts) {
      if (fact.empty()) continue;
      out.invalidated_facts.push_back(PreparedInvalidatedFact{
          .original = fact,
          .lowered = LowerAscii(fact),
      });
    }
    if (!out.invalidated_facts.empty()) {
      prepared.push_back(std::move(out));
    }
  }
  return prepared;
}

MatchedRevocation MatchRevocationForEvent(
    const Event& event, uint64_t event_index,
    const std::vector<PreparedCorrectionDirective>& directives) {
  MatchedRevocation match;
  if (event.type == Event::Type::kCorrection) return match;
  const std::string lowered_payload = LowerAscii(event.payload);
  for (const PreparedCorrectionDirective& directive : directives) {
    if (!CorrectionAppliesToEvent(event_index, directive)) continue;
    bool matched_directive = false;
    for (const PreparedInvalidatedFact& fact : directive.invalidated_facts) {
      if (lowered_payload.find(fact.lowered) == std::string::npos) {
        continue;
      }
      match.invalidated_facts.push_back(fact.original);
      matched_directive = true;
    }
    if (matched_directive) {
      match.correction_event_ids.push_back(directive.correction_event_id);
    }
  }
  return match;
}

Event RevokedEvidenceEvent(
    const Event& event, const RevokedEvidenceRecord& record) {
  Event revoked = event;
  revoked.type = Event::Type::kInternal;
  revoked.payload = absl::StrCat(
      "REVOKED_BY_CORRECTION: this event contained ",
      record.invalidated_facts.size(),
      " invalidated fact(s) and is not active evidence for the projection. "
      "Apply correction_id(s): ",
      record.correction_event_ids.empty()
          ? "unknown"
          : absl::StrJoin(record.correction_event_ids, ","),
      ". Do not reconstruct the original revoked wording from this marker.");
  return revoked;
}

void AppendRevokedEvidenceLogRecord(const RevokedEvidenceRecord& record,
                                    std::string* revoked_evidence_log) {
  if (!revoked_evidence_log->empty()) revoked_evidence_log->push_back('\n');
  absl::StrAppend(
      revoked_evidence_log, "[", record.global_event_index + 1, "] ",
      "revoked_by=", absl::StrJoin(record.correction_event_ids, ","),
      " invalidated_facts=", absl::StrJoin(record.invalidated_facts, " | "),
      " original=", record.original_event_json);
}

}  // namespace

absl::StatusOr<ActiveEvidenceView> BuildActiveEvidenceView(
    const EventSourcedLog& log, uint64_t event_range_start,
    uint64_t event_range_end,
    const std::vector<ProjectionCorrectionDirective>& correction_directives) {
  ASSIGN_OR_RETURN(std::vector<Event> events, log.GetAllEvents());
  return BuildActiveEvidenceViewFromEvents(events, event_range_start,
                                           event_range_end,
                                           correction_directives);
}

absl::StatusOr<ActiveEvidenceView> BuildActiveEvidenceViewFromEvents(
    const std::vector<Event>& events, uint64_t event_range_start,
    uint64_t event_range_end,
    const std::vector<ProjectionCorrectionDirective>& correction_directives) {
  if (event_range_end < event_range_start) {
    return absl::InvalidArgumentError("DPM projection event range is inverted.");
  }
  if (event_range_end > events.size()) {
    return absl::InvalidArgumentError(
        "DPM projection event range exceeds log generation.");
  }

  const std::vector<PreparedCorrectionDirective> prepared_directives =
      PrepareCorrectionDirectives(correction_directives);
  ActiveEvidenceView view;
  view.event_range_start = event_range_start;
  view.event_range_end = event_range_end;
  for (uint64_t i = event_range_start; i < event_range_end; ++i) {
    if (!view.active_event_log.empty()) view.active_event_log.push_back('\n');
    const Event& event = events[static_cast<size_t>(i)];
    const MatchedRevocation revocation =
        MatchRevocationForEvent(event, i, prepared_directives);
    if (revocation.invalidated_facts.empty()) {
      absl::StrAppend(&view.active_event_log, "[", i + 1, "] ",
                      EventToJsonLine(event));
      continue;
    }

    RevokedEvidenceRecord record{
        .global_event_index = i,
        .correction_event_ids = revocation.correction_event_ids,
        .invalidated_facts = revocation.invalidated_facts,
        .original_event_json = EventToJsonLine(event),
    };
    const Event rendered = RevokedEvidenceEvent(event, record);
    absl::StrAppend(&view.active_event_log, "[", i + 1, "] ",
                    EventToJsonLine(rendered));
    AppendRevokedEvidenceLogRecord(record, &view.revoked_evidence_log);
    view.revoked_records.push_back(std::move(record));
  }
  return view;
}

}  // namespace litert::lm
