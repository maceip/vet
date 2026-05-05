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

#include "runtime/platform/audit/audit_certificate_signer.h"

#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/audit/audit_certificate.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {
namespace {

bool AlgorithmAllowed(absl::string_view algorithm,
                      const std::vector<std::string>& allowed_algorithms) {
  if (allowed_algorithms.empty()) return true;
  for (const std::string& allowed : allowed_algorithms) {
    if (algorithm == allowed) return true;
  }
  return false;
}

}  // namespace

absl::StatusOr<AuditCertificate> AddAuditCertificateSignature(
    AuditCertificate certificate, const AuditCertificateSigner& signer) {
  if (signer.Algorithm().empty() || signer.KeyId().empty()) {
    return absl::InvalidArgumentError(
        "audit certificate signer requires algorithm and key_id.");
  }
  ASSIGN_OR_RETURN(std::string canonical,
                   EncodeCanonicalAuditCertificate(certificate));
  ASSIGN_OR_RETURN(std::string signature_bytes, signer.Sign(canonical));
  if (signature_bytes.empty()) {
    return absl::InvalidArgumentError(
        "audit certificate signer returned empty signature.");
  }
  certificate.signatures.push_back(AuditCertificateSignature{
      .algorithm = std::string(signer.Algorithm()),
      .key_id = std::string(signer.KeyId()),
      .signature = std::move(signature_bytes),
  });
  return FinalizeAuditCertificate(std::move(certificate));
}

absl::Status VerifyAuditCertificateSignature(
    const AuditCertificate& certificate,
    const AuditCertificateSignature& signature,
    const AuditCertificateVerifier& verifier) {
  if (signature.algorithm.empty() || signature.key_id.empty() ||
      signature.signature.empty()) {
    return absl::InvalidArgumentError(
        "audit certificate signature requires algorithm, key_id, and bytes.");
  }
  ASSIGN_OR_RETURN(std::string canonical,
                   EncodeCanonicalAuditCertificate(certificate));
  return verifier.Verify(signature, canonical);
}

absl::StatusOr<int> CountValidAuditCertificateSignatures(
    const AuditCertificate& certificate,
    const std::vector<std::string>& allowed_algorithms,
    const AuditCertificateVerifier& verifier) {
  int valid = 0;
  for (const AuditCertificateSignature& signature : certificate.signatures) {
    if (!AlgorithmAllowed(signature.algorithm, allowed_algorithms)) continue;
    if (VerifyAuditCertificateSignature(certificate, signature, verifier).ok()) {
      ++valid;
    }
  }
  return valid;
}

}  // namespace litert::lm
