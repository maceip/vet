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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_CORRECTION_PROTOCOL_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_CORRECTION_PROTOCOL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/dpm/event.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/dpm/projection_prompt.h"
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {

enum class CorrectionSeverity {
  kInfo,
  kWarning,
  kBlocking,
};

struct CorrectionPayload {
  std::string correction_id;
  Hash256 target_checkpoint_manifest_hash;
  uint64_t target_event_range_start = 0;
  uint64_t target_event_range_end = 0;
  Hash256 audit_certificate_id;
  std::string reason_code;
  CorrectionSeverity severity = CorrectionSeverity::kBlocking;
  std::vector<std::string> drift_fields;
  std::vector<Hash256> invalidates_checkpoints;
  std::string correction_text;
  std::vector<std::string> invalidated_facts;
  std::vector<std::string> replacement_facts;
  ProjectionCorrectionScope scope =
      ProjectionCorrectionScope::kCheckpointRange;
  std::string replacement_projection;
  bool must_interrupt_before_next_predict = true;
  int64_t created_unix_micros = 0;
};

absl::string_view CorrectionSeverityToString(CorrectionSeverity severity);
absl::StatusOr<CorrectionSeverity> CorrectionSeverityFromString(
    absl::string_view severity);

absl::Status ValidateCorrectionPayload(const CorrectionPayload& payload);
std::string CorrectionPayloadToJson(const CorrectionPayload& payload);
absl::StatusOr<CorrectionPayload> CorrectionPayloadFromJson(
    absl::string_view json);

// Compiles persisted correction events into the machine-actionable replay
// directives consumed by the projector. Blocking corrections that interrupt
// prediction must carry either invalidated facts, replacement facts, or a
// replacement projection; otherwise replay would be prompt-only policy text and
// the caller should fail closed.
absl::StatusOr<std::vector<ProjectionCorrectionDirective>>
CompileProjectionCorrectionDirectives(
    const std::vector<CorrectionPayload>& corrections);

// Compatibility shim for older call sites/tests that only need best-effort
// rendering. Decision-time replay should use CompileProjectionCorrectionDirectives.
std::vector<ProjectionCorrectionDirective> BuildProjectionCorrectionDirectives(
    const std::vector<CorrectionPayload>& corrections);

absl::Status AppendCorrectionEvent(EventSourcedLog* log,
                                   const CorrectionPayload& payload);

class CorrectionIndex {
 public:
  static absl::StatusOr<CorrectionIndex> Build(
      const std::vector<Event>& events);
  static absl::StatusOr<CorrectionIndex> Build(const EventSourcedLog& log);
  static absl::StatusOr<CorrectionIndex> LoadForCheckpoint(
      const EventSourcedLog& log, const Hash256& checkpoint_manifest_hash);

  bool HasBlockingCorrectionFor(const Hash256& checkpoint_manifest_hash) const;
  std::vector<CorrectionPayload> BlockingCorrectionsFor(
      const Hash256& checkpoint_manifest_hash) const;

 private:
  std::vector<CorrectionPayload> corrections_;
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_CORRECTION_PROTOCOL_H_
