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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_AUDIT_LIBOQS_AUDIT_CERTIFICATE_SIGNER_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_AUDIT_LIBOQS_AUDIT_CERTIFICATE_SIGNER_H_

#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/audit/audit_certificate_signer.h"

namespace litert::lm {

struct LibOqsSignatureKeyPair {
  std::string algorithm;
  std::string key_id;
  std::string public_key;
  std::string secret_key;
};

absl::StatusOr<LibOqsSignatureKeyPair> GenerateLibOqsMlDsaKeyPair(
    absl::string_view algorithm, absl::string_view key_id,
    absl::string_view library_path = "");

class LibOqsAuditCertificateSigner : public AuditCertificateSigner {
 public:
  explicit LibOqsAuditCertificateSigner(LibOqsSignatureKeyPair key_pair,
                                        absl::string_view library_path = "");

  absl::string_view Algorithm() const override;
  absl::string_view KeyId() const override;
  absl::StatusOr<std::string> Sign(
      absl::string_view canonical_certificate) const override;

 private:
  LibOqsSignatureKeyPair key_pair_;
  std::string library_path_;
};

class LibOqsAuditCertificateVerifier : public AuditCertificateVerifier {
 public:
  explicit LibOqsAuditCertificateVerifier(absl::string_view library_path = "");

  absl::Status AddPublicKey(absl::string_view algorithm,
                            absl::string_view key_id,
                            absl::string_view public_key);
  absl::Status Verify(
      const AuditCertificateSignature& signature,
      absl::string_view canonical_certificate) const override;

 private:
  std::string library_path_;
  struct PublicKey {
    std::string algorithm;
    std::string key_id;
    std::string bytes;
  };
  std::vector<PublicKey> public_keys_;
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_AUDIT_LIBOQS_AUDIT_CERTIFICATE_SIGNER_H_
