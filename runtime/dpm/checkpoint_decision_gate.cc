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

#include "runtime/dpm/checkpoint_decision_gate.h"

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "runtime/dpm/correction_protocol.h"
#include "runtime/platform/audit/audit_certificate.h"
#include "runtime/platform/audit/audit_ledger.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {

absl::StatusOr<CheckpointDecisionGateResult> MayUseCheckpointForDecision(
    const CheckpointDecisionGateRequest& request, const AuditLedger& ledger,
    const CorrectionIndex& corrections) {
  if (request.identity.tenant_id.empty() || request.identity.session_id.empty()) {
    return absl::InvalidArgumentError("decision gate requires log identity.");
  }
  if (!request.compatibility_ok) {
    return CheckpointDecisionGateResult{
        .may_use = false,
        .reason = "checkpoint compatibility failed",
    };
  }
  if (!request.thaw_verification_ok) {
    return CheckpointDecisionGateResult{
        .may_use = false,
        .reason = "checkpoint thaw verification failed",
    };
  }
  if (corrections.HasBlockingCorrectionFor(request.checkpoint_manifest_hash)) {
    return CheckpointDecisionGateResult{
        .may_use = false,
        .reason = "blocking correction invalidates checkpoint",
    };
  }
  absl::StatusOr<AuditCertificate> certificate_or =
      ledger.LatestForCheckpoint(request.identity.tenant_id,
                                 request.identity.session_id,
                                 request.checkpoint_manifest_hash);
  if (!certificate_or.ok()) {
    if (certificate_or.status().code() == absl::StatusCode::kNotFound) {
      return CheckpointDecisionGateResult{
          .may_use = false,
          .reason = "checkpoint has no audit certificate",
      };
    }
    return certificate_or.status();
  }
  const AuditCertificate& certificate = *certificate_or;
  if (certificate.verdict != AuditVerdict::kPass) {
    return CheckpointDecisionGateResult{
        .may_use = false,
        .reason = "checkpoint audit verdict is not pass",
    };
  }
  if (certificate.event_range_start != 0 ||
      certificate.event_range_end < request.checkpoint_event_count) {
    return CheckpointDecisionGateResult{
        .may_use = false,
        .reason = "audit certificate does not cover checkpoint event range",
    };
  }
  if (certificate.drift_score != 0.0) {
    return CheckpointDecisionGateResult{
        .may_use = false,
        .reason = "audit certificate reports projection drift",
    };
  }
  return CheckpointDecisionGateResult{
      .may_use = true,
      .reason = "checkpoint passed compatibility, thaw, audit, correction gate",
  };
}

}  // namespace litert::lm
