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

#include "runtime/platform/audit/audit_query.h"

#include <cmath>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/audit/audit_certificate.h"
#include "runtime/platform/audit/audit_ledger.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {
namespace {

absl::Status ValidateOptions(const AuditQueryOptions& options) {
  if (std::isnan(options.max_allowed_drift_score) ||
      options.max_allowed_drift_score < 0.0 ||
      options.max_allowed_drift_score > 1.0) {
    return absl::InvalidArgumentError(
        "audit query max_allowed_drift_score must be in [0.0, 1.0].");
  }
  return absl::OkStatus();
}

CheckpointAuditSummary SummarizeLatest(
    const Hash256& checkpoint_hash, const AuditCertificate& certificate,
    const AuditQueryOptions& options) {
  CheckpointAuditSummary summary;
  summary.checkpoint_manifest_hash = checkpoint_hash;
  summary.has_certificate = true;
  summary.latest_verdict = certificate.verdict;
  summary.latest_drift_score = certificate.drift_score;
  if (certificate.verdict == AuditVerdict::kPending) {
    summary.reason = "audit pending";
    return summary;
  }
  if (certificate.verdict != AuditVerdict::kPass) {
    summary.reason = "latest audit verdict is not pass";
    return summary;
  }
  if (certificate.drift_score > options.max_allowed_drift_score) {
    summary.reason = "latest audit drift score exceeds threshold";
    return summary;
  }
  summary.has_passing_audit = true;
  summary.reason = "latest audit passed";
  return summary;
}

}  // namespace

absl::StatusOr<std::vector<CheckpointAuditSummary>>
SummarizeCheckpointAudits(absl::string_view tenant_id,
                          absl::string_view session_id,
                          const std::vector<Hash256>& checkpoint_hashes,
                          const AuditLedger& ledger,
                          AuditQueryOptions options) {
  RETURN_IF_ERROR(ValidateOptions(options));
  std::vector<CheckpointAuditSummary> summaries;
  summaries.reserve(checkpoint_hashes.size());
  for (const Hash256& checkpoint_hash : checkpoint_hashes) {
    absl::StatusOr<AuditCertificate> certificate =
        ledger.LatestForCheckpoint(tenant_id, session_id, checkpoint_hash);
    if (!certificate.ok()) {
      if (certificate.status().code() != absl::StatusCode::kNotFound) {
        return certificate.status();
      }
      CheckpointAuditSummary summary;
      summary.checkpoint_manifest_hash = checkpoint_hash;
      summary.reason = "no audit certificate";
      summaries.push_back(summary);
      continue;
    }
    summaries.push_back(
        SummarizeLatest(checkpoint_hash, *certificate, options));
  }
  return summaries;
}

absl::StatusOr<std::vector<Hash256>> FindCheckpointsWithoutPassingAudit(
    absl::string_view tenant_id, absl::string_view session_id,
    const std::vector<Hash256>& checkpoint_hashes,
    const AuditLedger& ledger, AuditQueryOptions options) {
  ASSIGN_OR_RETURN(
      std::vector<CheckpointAuditSummary> summaries,
      SummarizeCheckpointAudits(tenant_id, session_id, checkpoint_hashes,
                                ledger, options));
  std::vector<Hash256> out;
  for (const CheckpointAuditSummary& summary : summaries) {
    if (!summary.has_passing_audit) {
      out.push_back(summary.checkpoint_manifest_hash);
    }
  }
  return out;
}

}  // namespace litert::lm
