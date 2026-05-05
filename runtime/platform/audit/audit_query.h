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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_AUDIT_AUDIT_QUERY_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_AUDIT_AUDIT_QUERY_H_

#include <string>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/audit/audit_certificate.h"
#include "runtime/platform/audit/audit_ledger.h"
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {

struct AuditQueryOptions {
  double max_allowed_drift_score = 0.0;
};

struct CheckpointAuditSummary {
  Hash256 checkpoint_manifest_hash;
  bool has_certificate = false;
  AuditVerdict latest_verdict = AuditVerdict::kInconclusive;
  double latest_drift_score = 1.0;
  bool has_passing_audit = false;
  std::string reason;
};

absl::StatusOr<std::vector<CheckpointAuditSummary>>
SummarizeCheckpointAudits(absl::string_view tenant_id,
                          absl::string_view session_id,
                          const std::vector<Hash256>& checkpoint_hashes,
                          const AuditLedger& ledger,
                          AuditQueryOptions options = {});

absl::StatusOr<std::vector<Hash256>> FindCheckpointsWithoutPassingAudit(
    absl::string_view tenant_id, absl::string_view session_id,
    const std::vector<Hash256>& checkpoint_hashes,
    const AuditLedger& ledger, AuditQueryOptions options = {});

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_AUDIT_AUDIT_QUERY_H_
