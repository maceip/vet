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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_PROJECTION_REPLAY_AUDITOR_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_PROJECTION_REPLAY_AUDITOR_H_

#include <cstdint>
#include <optional>
#include <string>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "runtime/dpm/checkpointed_projection.h"
#include "runtime/dpm/correction_protocol.h"
#include "runtime/dpm/dpm_projector.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/platform/audit/audit_certificate.h"
#include "runtime/platform/audit/audit_ledger.h"
#include "runtime/platform/checkpoint/checkpoint_store.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/platform/provenance/merkle_dag_store.h"

namespace litert::lm {

struct ProjectionReplayAuditConfig {
  ProjectionCheckpointConfig checkpoint;
  std::string auditor_model_id = "dpm-exact-replay-auditor";
  std::string audit_policy_version = "exact-replay-v1";
  int64_t created_unix_micros = 0;
  bool emit_correction_on_drift = true;
};

struct ProjectionReplayAuditResult {
  AuditCertificate certificate;
  std::optional<CorrectionPayload> correction;
  std::string replayed_projection;
};

// Replays the raw event log through the configured DPM projector, compares the
// replayed projection's BLAKE3 body hash with the checkpoint manifest's body
// hash, writes an AuditCertificate to the ledger, and records the certificate
// as a child node of the checkpoint in the Merkle DAG. The exact-replay v1
// policy is intentionally binary: drift_score is 0.0 iff bytes match, 1.0 iff
// the replayed projection body hash differs.
//
// This is the deterministic Phase 3 verifier. Semantic/LLM auditors can emit
// the same AuditCertificate shape later, but the trust loop starts here.
absl::StatusOr<ProjectionReplayAuditResult> VerifyProjectionCheckpointFromRaw(
    const EventSourcedLog& log, DPMProjector* projector,
    const Hash256& checkpoint_manifest_hash,
    const ProjectionReplayAuditConfig& config, CheckpointStore* store,
    AuditLedger* ledger, MerkleDagStore* dag);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_PROJECTION_REPLAY_AUDITOR_H_
