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

#include "runtime/dpm/audited_checkpoint_loader.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "runtime/dpm/active_evidence_view.h"
#include "runtime/dpm/checkpoint_decision_gate.h"
#include "runtime/dpm/checkpointed_projection.h"
#include "runtime/dpm/correction_protocol.h"
#include "runtime/dpm/dpm_projector.h"
#include "runtime/dpm/event.h"
#include "runtime/platform/audit/audit_ledger.h"
#include "runtime/platform/checkpoint/checkpoint_store.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {
namespace {

CheckpointDecisionGateRequest GateRequestFrom(
    const AuditedProjectionCheckpointRequest& request) {
  return CheckpointDecisionGateRequest{
      .identity = request.identity,
      .checkpoint_manifest_hash = request.checkpoint_manifest_hash,
      .checkpoint_event_range_start = request.checkpoint_event_range_start,
      .checkpoint_event_range_end = request.checkpoint_event_range_end,
      .checkpoint_event_count = request.checkpoint_event_count,
      .compatibility_ok = request.compatibility_ok,
      .thaw_verification_ok = request.thaw_verification_ok,
      .max_allowed_drift_score = request.max_allowed_drift_score,
      .require_valid_signature = request.require_valid_signature,
      .min_valid_signatures = request.min_valid_signatures,
      .allowed_signature_algorithms = request.allowed_signature_algorithms,
  };
}

}  // namespace

absl::StatusOr<AuditedProjectionCheckpoint>
LoadAuditedProjectionCheckpointForDecision(
    const AuditedProjectionCheckpointRequest& request, CheckpointStore* store,
    const AuditLedger& ledger, const CorrectionIndex& corrections) {
  if (store == nullptr) {
    return absl::InvalidArgumentError("CheckpointStore is required.");
  }
  ASSIGN_OR_RETURN(CheckpointDecisionGateResult gate,
                   MayUseCheckpointForDecision(
                       GateRequestFrom(request), ledger, corrections,
                       request.signature_verifier));
  if (!gate.may_use) {
    return absl::FailedPreconditionError(
        absl::StrCat("checkpoint cannot be used for decision: ",
                     gate.reason));
  }
  ASSIGN_OR_RETURN(std::string projected_memory,
                   LoadProjectionCheckpoint(request.identity,
                                            request.checkpoint_manifest_hash,
                                            store));
  return AuditedProjectionCheckpoint{
      .projected_memory = std::move(projected_memory),
      .gate = std::move(gate),
  };
}

absl::StatusOr<CorrectionAwareCheckpointReplay>
LoadOrReplayAuditedProjectionCheckpointForDecision(
    const EventSourcedLog& log, DPMProjector* projector,
    const CorrectionAwareCheckpointReplayRequest& request,
    CheckpointStore* store, const AuditLedger& ledger,
    const CorrectionIndex& corrections) {
  if (projector == nullptr) {
    return absl::InvalidArgumentError("DPM projector is required.");
  }
  if (store == nullptr) {
    return absl::InvalidArgumentError("CheckpointStore is required.");
  }
  ASSIGN_OR_RETURN(CheckpointDecisionGateResult gate,
                   MayUseCheckpointForDecision(
                       GateRequestFrom(request.checkpoint), ledger,
                       corrections,
                       request.checkpoint.signature_verifier));
  if (gate.may_use) {
    ASSIGN_OR_RETURN(
        std::string projected_memory,
        LoadProjectionCheckpoint(request.checkpoint.identity,
                                 request.checkpoint.checkpoint_manifest_hash,
                                 store));
    return CorrectionAwareCheckpointReplay{
        .projected_memory = std::move(projected_memory),
        .gate = std::move(gate),
        .replayed_from_raw = false,
    };
  }

  const CorrectionBarrierDecision barrier = EvaluateCorrectionBarrier(
      request.checkpoint.checkpoint_manifest_hash, corrections);
  if (!barrier.must_reproject || barrier.blocking_corrections.empty()) {
    return absl::FailedPreconditionError(
        absl::StrCat("checkpoint cannot be used for decision: ",
                     gate.reason));
  }
  ASSIGN_OR_RETURN(std::vector<ProjectionCorrectionDirective> directives,
                   CompileProjectionCorrectionDirectives(
                       barrier.blocking_corrections));
  ASSIGN_OR_RETURN(std::vector<Event> events, log.GetAllEvents());
  const uint64_t replay_end =
      request.replay_event_range_end == 0 ? events.size()
                                          : request.replay_event_range_end;
  if (replay_end > events.size()) {
    return absl::InvalidArgumentError(
        "correction-aware replay range exceeds log generation.");
  }
  ASSIGN_OR_RETURN(ActiveEvidenceView active_evidence_view,
                   BuildActiveEvidenceViewFromEvents(
                       events, request.replay_event_range_start, replay_end,
                       directives));
  ASSIGN_OR_RETURN(
      std::string projected_memory,
      projector->ProjectActiveEvidenceView(active_evidence_view,
                                           request.projection, directives));
  return CorrectionAwareCheckpointReplay{
      .projected_memory = std::move(projected_memory),
      .gate = std::move(gate),
      .replayed_from_raw = true,
      .correction_directives = std::move(directives),
      .active_evidence_view = std::move(active_evidence_view),
  };
}

absl::StatusOr<CorrectionAwareCheckpointReplay>
LoadOrReplayAuditedProjectionCheckpointForDecision(
    const EventSourcedLog& log, DPMProjector* projector,
    const CorrectionAwareCheckpointReplayRequest& request,
    CheckpointStore* store, const AuditLedger& ledger) {
  ASSIGN_OR_RETURN(
      CorrectionIndex corrections,
      CorrectionIndex::LoadForCheckpoint(
          log, request.checkpoint.checkpoint_manifest_hash));
  return LoadOrReplayAuditedProjectionCheckpointForDecision(
      log, projector, request, store, ledger, corrections);
}

}  // namespace litert::lm
