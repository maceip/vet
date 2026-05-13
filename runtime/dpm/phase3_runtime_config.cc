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

#include "runtime/dpm/phase3_runtime_config.h"

#include <cmath>

#include "absl/status/status.h"  // from @com_google_absl

namespace litert::lm {

absl::Status ValidatePhase3RuntimeConfig(const Phase3RuntimeConfig& config) {
  if (std::isnan(config.audit_gate.max_allowed_drift_score) ||
      config.audit_gate.max_allowed_drift_score < 0.0 ||
      config.audit_gate.max_allowed_drift_score > 1.0) {
    return absl::InvalidArgumentError(
        "Phase3RuntimeConfig audit_gate.max_allowed_drift_score must be in "
        "[0.0, 1.0].");
  }
  if (config.audit_gate.min_valid_signatures < 1) {
    return absl::InvalidArgumentError(
        "Phase3RuntimeConfig audit_gate.min_valid_signatures must be at "
        "least 1.");
  }
  if (config.audit_gate.require_valid_signature &&
      config.audit_gate.allowed_signature_algorithms.empty()) {
    return absl::InvalidArgumentError(
        "Phase3RuntimeConfig requires explicit allowed signature algorithms "
        "when signature validation is enabled.");
  }
  if (config.correction_replay.correction_repair_attempts < 0) {
    return absl::InvalidArgumentError(
        "Phase3RuntimeConfig correction_replay.correction_repair_attempts "
        "must be non-negative.");
  }
  if (!config.correction_replay.require_machine_actionable_blocking_corrections) {
    return absl::InvalidArgumentError(
        "Phase3RuntimeConfig must require machine-actionable blocking "
        "corrections for decision-time replay.");
  }
  return absl::OkStatus();
}

absl::Status ApplyPhase3RuntimeConfig(
    const Phase3RuntimeConfig& config,
    AuditedProjectionCheckpointRequest* checkpoint_request,
    DPMProjector::ProjectionConfig* projection_config) {
  if (checkpoint_request == nullptr || projection_config == nullptr) {
    return absl::InvalidArgumentError(
        "ApplyPhase3RuntimeConfig requires checkpoint and projection configs.");
  }
  const absl::Status status = ValidatePhase3RuntimeConfig(config);
  if (!status.ok()) return status;

  checkpoint_request->max_allowed_drift_score =
      config.audit_gate.max_allowed_drift_score;
  checkpoint_request->require_valid_signature =
      config.audit_gate.require_valid_signature;
  checkpoint_request->min_valid_signatures =
      config.audit_gate.min_valid_signatures;
  checkpoint_request->allowed_signature_algorithms =
      config.audit_gate.allowed_signature_algorithms;
  projection_config->correction_repair_attempts =
      config.correction_replay.correction_repair_attempts;
  return absl::OkStatus();
}

}  // namespace litert::lm
