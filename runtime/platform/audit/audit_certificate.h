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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_AUDIT_AUDIT_CERTIFICATE_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_AUDIT_AUDIT_CERTIFICATE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {

enum class AuditVerdict {
  kPass,
  kCorrectionEmitted,
  kInconclusive,
};

// Closed drift score contract for Phase 3:
//   * score is in [0.0, 1.0]
//   * 0.0 means the comparator detected no projection drift
//   * 1.0 means complete audit failure
//   * the algorithm is identified by audit_policy_version
//
// The substrate does not prescribe an LLM judge or embedding model. Tests use
// exact and structured comparators; semantic auditors can write certificates
// with the same contract later.
struct AuditCertificate {
  Hash256 certificate_id;
  Hash256 checkpoint_manifest_hash;
  Hash256 checkpoint_body_hash;
  std::string tenant_id;
  std::string session_id;
  std::string branch_id;
  uint64_t event_range_start = 0;
  uint64_t event_range_end = 0;
  uint64_t log_generation = 0;
  std::string schema_id;
  Hash256 model_artifact_hash;
  std::string projection_model_id;
  std::string auditor_model_id;
  std::string audit_policy_version;
  AuditVerdict verdict = AuditVerdict::kInconclusive;
  double drift_score = 1.0;
  std::vector<std::string> drift_fields;
  std::vector<std::string> correction_event_ids;
  Hash256 provenance_root_hash;
  int64_t created_unix_micros = 0;
  int64_t expires_unix_micros = 0;
  std::string signature;
};

inline constexpr uint32_t kAuditCertificateVersion = 1;

absl::string_view AuditVerdictToString(AuditVerdict verdict);
absl::StatusOr<AuditVerdict> AuditVerdictFromString(
    absl::string_view verdict);

absl::Status ValidateAuditCertificateForHashing(
    const AuditCertificate& certificate);

// Canonical bytes exclude certificate_id and signature. certificate_id is the
// BLAKE3 digest of these bytes; signature is a deployment-layer attestation over
// the same bytes and can be added without changing identity.
absl::StatusOr<std::string> EncodeCanonicalAuditCertificate(
    const AuditCertificate& certificate);
absl::StatusOr<AuditCertificate> DecodeCanonicalAuditCertificate(
    absl::string_view bytes);
absl::StatusOr<Hash256> ComputeAuditCertificateId(
    const AuditCertificate& certificate);
absl::StatusOr<AuditCertificate> FinalizeAuditCertificate(
    AuditCertificate certificate);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_AUDIT_AUDIT_CERTIFICATE_H_
