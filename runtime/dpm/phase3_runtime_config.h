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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_PHASE3_RUNTIME_CONFIG_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_PHASE3_RUNTIME_CONFIG_H_

#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "runtime/dpm/audited_checkpoint_loader.h"
#include "runtime/dpm/dpm_projector.h"

namespace litert::lm {

struct Phase3AuditGatePolicy {
  // Exact-replay v1 is byte-strict by default. Structured/semantic auditors can
  // raise this only after their comparator defines drift_score semantics.
  double max_allowed_drift_score = 0.0;
  bool require_valid_signature = false;
  int min_valid_signatures = 1;
  std::vector<std::string> allowed_signature_algorithms;
};

struct Phase3CorrectionReplayPolicy {
  int correction_repair_attempts = 1;
  bool require_machine_actionable_blocking_corrections = true;
};

struct Phase3RuntimeConfig {
  Phase3AuditGatePolicy audit_gate;
  Phase3CorrectionReplayPolicy correction_replay;
};

absl::Status ValidatePhase3RuntimeConfig(const Phase3RuntimeConfig& config);

// Applies runtime policy to the two request/config structs that feed the live
// decision path. This keeps gate thresholds, signature policy, and correction
// repair behavior from being hand-assembled at each call site.
absl::Status ApplyPhase3RuntimeConfig(
    const Phase3RuntimeConfig& config,
    AuditedProjectionCheckpointRequest* checkpoint_request,
    DPMProjector::ProjectionConfig* projection_config);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_PHASE3_RUNTIME_CONFIG_H_
