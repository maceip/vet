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
#include "runtime/dpm/event_sourced_log.h"
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

// Runtime enforcement hook for Phase 3: callers that want to use a checkpoint
// as decision memory must pass through the audit/correction gate first. Missing
// audit, pending audit, failed thaw, drift, and blocking corrections all fail
// closed and force the caller to re-project from the raw event log.
absl::StatusOr<AuditedProjectionCheckpoint>
LoadAuditedProjectionCheckpointForDecision(
    const AuditedProjectionCheckpointRequest& request, CheckpointStore* store,
    const AuditLedger& ledger, const CorrectionIndex& corrections);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_AUDITED_CHECKPOINT_LOADER_H_
