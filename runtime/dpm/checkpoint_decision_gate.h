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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_CHECKPOINT_DECISION_GATE_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_CHECKPOINT_DECISION_GATE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/dpm/correction_protocol.h"
#include "runtime/platform/audit/audit_ledger.h"
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {

struct CheckpointDecisionGateRequest {
  DPMLogIdentity identity;
  Hash256 checkpoint_manifest_hash;
  uint64_t checkpoint_event_count = 0;
  bool compatibility_ok = false;
  bool thaw_verification_ok = false;
};

struct CheckpointDecisionGateResult {
  bool may_use = false;
  std::string reason;
};

struct CorrectionBarrierDecision {
  bool must_interrupt_before_next_predict = false;
  bool must_reproject = false;
  std::string reason;
  std::vector<CorrectionPayload> blocking_corrections;
};

CorrectionBarrierDecision EvaluateCorrectionBarrier(
    const Hash256& active_checkpoint_manifest_hash,
    const CorrectionIndex& corrections);

absl::StatusOr<CheckpointDecisionGateResult> MayUseCheckpointForDecision(
    const CheckpointDecisionGateRequest& request, const AuditLedger& ledger,
    const CorrectionIndex& corrections);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_CHECKPOINT_DECISION_GATE_H_
