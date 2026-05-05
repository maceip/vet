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

#include <cmath>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "runtime/dpm/correction_protocol.h"
#include "runtime/platform/audit/audit_certificate.h"
#include "runtime/platform/audit/audit_certificate_signer.h"
#include "runtime/platform/audit/audit_ledger.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {

CorrectionBarrierDecision EvaluateCorrectionBarrier(
    const Hash256& active_checkpoint_manifest_hash,
    const CorrectionIndex& corrections) {
  std::vector<CorrectionPayload> blocking =
      corrections.BlockingCorrectionsFor(active_checkpoint_manifest_hash);
  if (blocking.empty()) {
    return CorrectionBarrierDecision{
        .must_interrupt_before_next_predict = false,
        .must_reproject = false,
        .reason = "no blocking correction for active checkpoint",
    };
  }
  bool interrupt = false;
  for (const CorrectionPayload& correction : blocking) {
    interrupt = interrupt || correction.must_interrupt_before_next_predict;
  }
  return CorrectionBarrierDecision{
      .must_interrupt_before_next_predict = interrupt,
      .must_reproject = true,
      .reason = "blocking correction requires fresh projection",
      .blocking_corrections = std::move(blocking),
  };
}

absl::StatusOr<CheckpointDecisionGateResult> MayUseCheckpointForDecision(
    const CheckpointDecisionGateRequest& request, const AuditLedger& ledger,
    const CorrectionIndex& corrections) {
  return MayUseCheckpointForDecision(request, ledger, corrections, nullptr);
}

absl::StatusOr<CheckpointDecisionGateResult> MayUseCheckpointForDecision(
    const CheckpointDecisionGateRequest& request, const AuditLedger& ledger,
    const CorrectionIndex& corrections,
    const AuditCertificateVerifier* signature_verifier) {
  if (request.identity.tenant_id.empty() || request.identity.session_id.empty()) {
    return absl::InvalidArgumentError("decision gate requires log identity.");
  }
  if (std::isnan(request.max_allowed_drift_score) ||
      request.max_allowed_drift_score < 0.0 ||
      request.max_allowed_drift_score > 1.0) {
    return absl::InvalidArgumentError(
        "decision gate max_allowed_drift_score must be in [0.0, 1.0].");
  }
  if (request.min_valid_signatures < 1) {
    return absl::InvalidArgumentError(
        "decision gate min_valid_signatures must be at least 1.");
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
  const CorrectionBarrierDecision barrier =
      EvaluateCorrectionBarrier(request.checkpoint_manifest_hash, corrections);
  if (barrier.must_reproject) {
    return CheckpointDecisionGateResult{
        .may_use = false,
        .reason = barrier.reason,
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
  if (certificate.verdict == AuditVerdict::kPending) {
    return CheckpointDecisionGateResult{
        .may_use = false,
        .reason = "checkpoint audit is pending",
    };
  }
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
        .reason =
            "exact-replay v1 requires a prefix audit covering events [0, "
            "checkpoint_event_count)",
    };
  }
  if (certificate.drift_score > request.max_allowed_drift_score) {
    return CheckpointDecisionGateResult{
        .may_use = false,
        .reason = "audit certificate reports projection drift",
    };
  }
  if (request.require_valid_signature) {
    if (signature_verifier == nullptr) {
      return CheckpointDecisionGateResult{
          .may_use = false,
          .reason = "checkpoint audit requires a signature verifier",
      };
    }
    ASSIGN_OR_RETURN(
        int valid_signatures,
        CountValidAuditCertificateSignatures(
            certificate, request.allowed_signature_algorithms,
            *signature_verifier));
    if (valid_signatures < request.min_valid_signatures) {
      return CheckpointDecisionGateResult{
          .may_use = false,
          .reason =
              "checkpoint audit certificate does not satisfy signature policy",
      };
    }
  }
  return CheckpointDecisionGateResult{
      .may_use = true,
      .reason = "checkpoint passed compatibility, thaw, audit, correction gate",
  };
}

}  // namespace litert::lm
