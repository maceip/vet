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

#include "runtime/platform/audit/local_filesystem_audit_ledger.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "runtime/platform/audit/audit_certificate.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

using ::testing::HasSubstr;

std::filesystem::path TestRoot(absl::string_view name) {
  std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / std::string(name);
  std::filesystem::remove_all(path);
  return path;
}

AuditCertificate CertificateFor(const Hash256& checkpoint_hash,
                                AuditVerdict verdict,
                                int64_t created_unix_micros) {
  AuditCertificate certificate;
  certificate.checkpoint_manifest_hash = checkpoint_hash;
  certificate.checkpoint_body_hash = HashBytes(
      HashAlgorithm::kBlake3,
      absl::StrCat("body:", checkpoint_hash.ToHex(), ":",
                   created_unix_micros));
  certificate.tenant_id = "tenant-a";
  certificate.session_id = "session-1";
  certificate.branch_id = "main";
  certificate.event_range_start = 0;
  certificate.event_range_end = 1;
  certificate.log_generation = 1;
  certificate.schema_id = "incident_response_v1";
  certificate.model_artifact_hash = HashBytes(HashAlgorithm::kBlake3, "model");
  certificate.projection_model_id = "pinned-projection-model";
  certificate.auditor_model_id = "local-ledger-test";
  certificate.audit_policy_version = "exact-replay-v1";
  certificate.verdict = verdict;
  certificate.drift_score =
      verdict == AuditVerdict::kPass ? 0.0 : 1.0;
  certificate.provenance_root_hash = checkpoint_hash;
  certificate.created_unix_micros = created_unix_micros;
  return certificate;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::string out;
  out.assign(std::istreambuf_iterator<char>(in),
             std::istreambuf_iterator<char>());
  return out;
}

TEST(LocalFilesystemAuditLedgerTest,
     LatestForCheckpointUsesDurableLatestIndex) {
  const std::filesystem::path root = TestRoot("audit_ledger_latest_index");
  LocalFilesystemAuditLedger ledger(root);
  const Hash256 checkpoint = HashBytes(HashAlgorithm::kBlake3, "checkpoint");

  ASSERT_OK(ledger.PutCertificate(
      CertificateFor(checkpoint, AuditVerdict::kPass, 100)));
  ASSERT_OK(ledger.PutCertificate(
      CertificateFor(checkpoint, AuditVerdict::kCorrectionEmitted, 200)));
  ASSERT_OK(ledger.PutCertificate(
      CertificateFor(checkpoint, AuditVerdict::kPending, 150)));

  ASSERT_OK_AND_ASSIGN(AuditCertificate latest,
                       ledger.LatestForCheckpoint("tenant-a", "session-1",
                                                  checkpoint));

  EXPECT_EQ(latest.created_unix_micros, 200);
  EXPECT_EQ(latest.verdict, AuditVerdict::kCorrectionEmitted);
  const std::filesystem::path index_path =
      ledger.LatestIndexPathFor("tenant-a", "session-1", checkpoint);
  ASSERT_TRUE(std::filesystem::exists(index_path));
  EXPECT_THAT(ReadFile(index_path), HasSubstr(latest.certificate_id.ToHex()));
}

TEST(LocalFilesystemAuditLedgerTest,
     LatestForCheckpointFallsBackWhenIndexIsMissing) {
  const std::filesystem::path root = TestRoot("audit_ledger_latest_fallback");
  LocalFilesystemAuditLedger ledger(root);
  const Hash256 checkpoint = HashBytes(HashAlgorithm::kBlake3, "checkpoint");

  ASSERT_OK(ledger.PutCertificate(
      CertificateFor(checkpoint, AuditVerdict::kPass, 100)));
  ASSERT_OK(ledger.PutCertificate(
      CertificateFor(checkpoint, AuditVerdict::kPending, 300)));
  std::filesystem::remove(
      ledger.LatestIndexPathFor("tenant-a", "session-1", checkpoint));

  ASSERT_OK_AND_ASSIGN(AuditCertificate latest,
                       ledger.LatestForCheckpoint("tenant-a", "session-1",
                                                  checkpoint));

  EXPECT_EQ(latest.created_unix_micros, 300);
  EXPECT_EQ(latest.verdict, AuditVerdict::kPending);
}

}  // namespace
}  // namespace litert::lm
