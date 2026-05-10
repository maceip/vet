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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_AUDITED_CHECKPOINT_LOADER_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_AUDITED_CHECKPOINT_LOADER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "runtime/dpm/checkpoint_decision_gate.h"
#include "runtime/dpm/checkpointed_projection.h"
#include "runtime/dpm/correction_protocol.h"
#include "runtime/dpm/dpm_projector.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/dpm/projection_prompt.h"
#include "runtime/platform/audit/audit_certificate_signer.h"
#include "runtime/platform/audit/audit_ledger.h"
#include "runtime/platform/checkpoint/checkpoint_store.h"
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {

struct AuditedProjectionCheckpointRequest {
  DPMLogIdentity identity;
  Hash256 checkpoint_manifest_hash;
  uint64_t checkpoint_event_range_start = 0;
  uint64_t checkpoint_event_range_end = 0;
  uint64_t checkpoint_event_count = 0;
  bool compatibility_ok = false;
  bool thaw_verification_ok = false;
  double max_allowed_drift_score = 0.0;
  bool require_valid_signature = false;
  int min_valid_signatures = 1;
  std::vector<std::string> allowed_signature_algorithms;
  const AuditCertificateVerifier* signature_verifier = nullptr;
};

struct AuditedProjectionCheckpoint {
  std::string projected_memory;
  CheckpointDecisionGateResult gate;
};

struct CorrectionAwareCheckpointReplayRequest {
  AuditedProjectionCheckpointRequest checkpoint;
  DPMProjector::ProjectionConfig projection;
  uint64_t replay_event_range_start = 0;
  // 0 means replay through the current append-only log generation.
  uint64_t replay_event_range_end = 0;
};

struct CorrectionAwareCheckpointReplay {
  std::string projected_memory;
  CheckpointDecisionGateResult gate;
  bool replayed_from_raw = false;
  std::vector<ProjectionCorrectionDirective> correction_directives;
};

// Runtime enforcement hook for Phase 3: callers that want to use a checkpoint
// as decision memory must pass through the audit/correction gate first. Missing
// audit, pending audit, failed thaw, drift, and blocking corrections all fail
// closed and force the caller to re-project from the raw event log.
absl::StatusOr<AuditedProjectionCheckpoint>
LoadAuditedProjectionCheckpointForDecision(
    const AuditedProjectionCheckpointRequest& request, CheckpointStore* store,
    const AuditLedger& ledger, const CorrectionIndex& corrections);

// Convenience recovery path for the common Phase 3 case:
//  1. Use the checkpoint if the gate accepts it.
//  2. If the gate refuses because a blocking correction invalidated it, replay
//     raw events through DPM with typed correction directives and deterministic
//     invalidated-fact guards.
//  3. Fail closed for every other refusal mode.
absl::StatusOr<CorrectionAwareCheckpointReplay>
LoadOrReplayAuditedProjectionCheckpointForDecision(
    const EventSourcedLog& log, DPMProjector* projector,
    const CorrectionAwareCheckpointReplayRequest& request,
    CheckpointStore* store, const AuditLedger& ledger,
    const CorrectionIndex& corrections);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_AUDITED_CHECKPOINT_LOADER_H_
