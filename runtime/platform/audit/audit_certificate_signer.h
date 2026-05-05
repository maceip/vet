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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_AUDIT_AUDIT_CERTIFICATE_SIGNER_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_AUDIT_AUDIT_CERTIFICATE_SIGNER_H_

#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/audit/audit_certificate.h"

namespace litert::lm {

class AuditCertificateSigner {
 public:
  virtual ~AuditCertificateSigner() = default;

  virtual absl::string_view Algorithm() const = 0;
  virtual absl::string_view KeyId() const = 0;
  virtual absl::StatusOr<std::string> Sign(
      absl::string_view canonical_certificate) const = 0;
};

class AuditCertificateVerifier {
 public:
  virtual ~AuditCertificateVerifier() = default;

  virtual absl::Status Verify(
      const AuditCertificateSignature& signature,
      absl::string_view canonical_certificate) const = 0;
};

absl::StatusOr<AuditCertificate> AddAuditCertificateSignature(
    AuditCertificate certificate, const AuditCertificateSigner& signer);

absl::Status VerifyAuditCertificateSignature(
    const AuditCertificate& certificate,
    const AuditCertificateSignature& signature,
    const AuditCertificateVerifier& verifier);

absl::StatusOr<int> CountValidAuditCertificateSignatures(
    const AuditCertificate& certificate,
    const std::vector<std::string>& allowed_algorithms,
    const AuditCertificateVerifier& verifier);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_AUDIT_AUDIT_CERTIFICATE_SIGNER_H_
