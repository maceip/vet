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

#include <filesystem>
#include <string>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gtest/gtest.h"
#include "runtime/platform/audit/audit_certificate.h"
#include "runtime/platform/audit/local_filesystem_audit_ledger.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

std::filesystem::path TestRoot(absl::string_view name) {
  std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / std::string(name);
  std::filesystem::remove_all(path);
  return path;
}

AuditCertificate CertificateFor(const Hash256& checkpoint_hash,
                                AuditVerdict verdict, double drift_score,
                                int64_t created_unix_micros) {
  AuditCertificate certificate;
  certificate.checkpoint_manifest_hash = checkpoint_hash;
  certificate.checkpoint_body_hash = HashBytes(
      HashAlgorithm::kBlake3,
      absl::StrCat("body:", checkpoint_hash.ToHex()));
  certificate.tenant_id = "tenant-a";
  certificate.session_id = "session-1";
  certificate.branch_id = "main";
  certificate.event_range_start = 0;
  certificate.event_range_end = 1;
  certificate.log_generation = 1;
  certificate.schema_id = "incident_response_v1";
  certificate.model_artifact_hash = HashBytes(HashAlgorithm::kBlake3, "model");
  certificate.projection_model_id = "pinned-projection-model";
  certificate.auditor_model_id = "audit-query-test";
  certificate.audit_policy_version = "exact-replay-v1";
  certificate.verdict = verdict;
  certificate.drift_score = drift_score;
  certificate.provenance_root_hash = checkpoint_hash;
  certificate.created_unix_micros = created_unix_micros;
  return certificate;
}

TEST(AuditQueryTest, FindsMissingPendingAndFailedCheckpoints) {
  LocalFilesystemAuditLedger ledger(TestRoot("audit_query"));
  const Hash256 pass = HashBytes(HashAlgorithm::kBlake3, "pass");
  const Hash256 pending = HashBytes(HashAlgorithm::kBlake3, "pending");
  const Hash256 drift = HashBytes(HashAlgorithm::kBlake3, "drift");
  const Hash256 missing = HashBytes(HashAlgorithm::kBlake3, "missing");

  ASSERT_OK(ledger.PutCertificate(
      CertificateFor(pass, AuditVerdict::kPass, 0.0, 100)));
  ASSERT_OK(ledger.PutCertificate(
      CertificateFor(pending, AuditVerdict::kPending, 1.0, 100)));
  ASSERT_OK(ledger.PutCertificate(
      CertificateFor(drift, AuditVerdict::kCorrectionEmitted, 1.0, 100)));

  ASSERT_OK_AND_ASSIGN(
      std::vector<CheckpointAuditSummary> summaries,
      SummarizeCheckpointAudits(
          "tenant-a", "session-1", {pass, pending, drift, missing}, ledger));
  ASSERT_EQ(summaries.size(), 4);
  EXPECT_TRUE(summaries[0].has_passing_audit);
  EXPECT_EQ(summaries[1].reason, "audit pending");
  EXPECT_EQ(summaries[2].reason, "latest audit verdict is not pass");
  EXPECT_EQ(summaries[3].reason, "no audit certificate");

  ASSERT_OK_AND_ASSIGN(
      std::vector<Hash256> failed,
      FindCheckpointsWithoutPassingAudit(
          "tenant-a", "session-1", {pass, pending, drift, missing}, ledger));
  EXPECT_EQ(failed, (std::vector<Hash256>{pending, drift, missing}));
}

TEST(AuditQueryTest, LatestCertificateControlsSummary) {
  LocalFilesystemAuditLedger ledger(TestRoot("audit_query_latest"));
  const Hash256 checkpoint = HashBytes(HashAlgorithm::kBlake3, "checkpoint");

  ASSERT_OK(ledger.PutCertificate(CertificateFor(
      checkpoint, AuditVerdict::kCorrectionEmitted, 1.0, 100)));
  ASSERT_OK(ledger.PutCertificate(
      CertificateFor(checkpoint, AuditVerdict::kPass, 0.0, 200)));

  ASSERT_OK_AND_ASSIGN(
      std::vector<Hash256> failed,
      FindCheckpointsWithoutPassingAudit(
          "tenant-a", "session-1", {checkpoint}, ledger));
  EXPECT_TRUE(failed.empty());
}

}  // namespace
}  // namespace litert::lm
