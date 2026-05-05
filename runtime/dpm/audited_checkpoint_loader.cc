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

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "runtime/dpm/checkpoint_decision_gate.h"
#include "runtime/dpm/checkpointed_projection.h"
#include "runtime/dpm/correction_protocol.h"
#include "runtime/platform/audit/audit_ledger.h"
#include "runtime/platform/checkpoint/checkpoint_store.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {

absl::StatusOr<AuditedProjectionCheckpoint>
LoadAuditedProjectionCheckpointForDecision(
    const AuditedProjectionCheckpointRequest& request, CheckpointStore* store,
    const AuditLedger& ledger, const CorrectionIndex& corrections) {
  if (store == nullptr) {
    return absl::InvalidArgumentError("CheckpointStore is required.");
  }
  CheckpointDecisionGateRequest gate_request{
      .identity = request.identity,
      .checkpoint_manifest_hash = request.checkpoint_manifest_hash,
      .checkpoint_event_count = request.checkpoint_event_count,
      .compatibility_ok = request.compatibility_ok,
      .thaw_verification_ok = request.thaw_verification_ok,
      .max_allowed_drift_score = request.max_allowed_drift_score,
  };
  ASSIGN_OR_RETURN(CheckpointDecisionGateResult gate,
                   MayUseCheckpointForDecision(gate_request, ledger,
                                               corrections));
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

}  // namespace litert::lm
