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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_AUDIT_LOCAL_FILESYSTEM_AUDIT_LEDGER_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_AUDIT_LOCAL_FILESYSTEM_AUDIT_LEDGER_H_

#include <filesystem>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/audit/audit_certificate.h"
#include "runtime/platform/audit/audit_ledger.h"
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {

class LocalFilesystemAuditLedger : public AuditLedger {
 public:
  explicit LocalFilesystemAuditLedger(std::filesystem::path root_path);

  absl::Status PutCertificate(
      const AuditCertificate& certificate) override;
  absl::StatusOr<AuditCertificate> GetCertificate(
      absl::string_view tenant_id, absl::string_view session_id,
      const Hash256& certificate_id) const override;
  absl::StatusOr<std::vector<AuditCertificate>> ListForCheckpoint(
      absl::string_view tenant_id, absl::string_view session_id,
      const Hash256& checkpoint_manifest_hash) const override;
  absl::StatusOr<std::vector<AuditCertificate>> ListForSession(
      absl::string_view tenant_id,
      absl::string_view session_id) const override;
  absl::StatusOr<AuditCertificate> LatestForCheckpoint(
      absl::string_view tenant_id, absl::string_view session_id,
      const Hash256& checkpoint_manifest_hash) const override;

  std::filesystem::path CertificatePathFor(absl::string_view tenant_id,
                                           absl::string_view session_id,
                                           const Hash256& certificate_id) const;
  std::filesystem::path LatestIndexPathFor(
      absl::string_view tenant_id, absl::string_view session_id,
      const Hash256& checkpoint_manifest_hash) const;

 private:
  std::filesystem::path root_path_;
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_AUDIT_LOCAL_FILESYSTEM_AUDIT_LEDGER_H_
