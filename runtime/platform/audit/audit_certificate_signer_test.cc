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

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "runtime/platform/audit/audit_certificate.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/platform/audit/local_filesystem_audit_ledger.h"
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

std::string TestSignature(absl::string_view algorithm,
                          absl::string_view key_id,
                          absl::string_view secret,
                          absl::string_view canonical) {
  return HashBytes(HashAlgorithm::kBlake3,
                   absl::StrCat(algorithm, "\n", key_id, "\n", secret, "\n",
                                canonical))
      .ToHex();
}

class TestOnlyMlDsaSigner : public AuditCertificateSigner {
 public:
  TestOnlyMlDsaSigner(std::string algorithm, std::string key_id,
                      std::string secret)
      : algorithm_(std::move(algorithm)),
        key_id_(std::move(key_id)),
        secret_(std::move(secret)) {}

  absl::string_view Algorithm() const override { return algorithm_; }
  absl::string_view KeyId() const override { return key_id_; }

  absl::StatusOr<std::string> Sign(
      absl::string_view canonical_certificate) const override {
    return TestSignature(algorithm_, key_id_, secret_, canonical_certificate);
  }

 private:
  std::string algorithm_;
  std::string key_id_;
  std::string secret_;
};

class TestOnlyMlDsaVerifier : public AuditCertificateVerifier {
 public:
  void AddKey(std::string algorithm, std::string key_id, std::string secret) {
    keys_.push_back(Key{
        .algorithm = std::move(algorithm),
        .key_id = std::move(key_id),
        .secret = std::move(secret),
    });
  }

  absl::Status Verify(
      const AuditCertificateSignature& signature,
      absl::string_view canonical_certificate) const override {
    for (const Key& key : keys_) {
      if (key.algorithm != signature.algorithm || key.key_id != signature.key_id) {
        continue;
      }
      if (signature.signature ==
          TestSignature(key.algorithm, key.key_id, key.secret,
                        canonical_certificate)) {
        return absl::OkStatus();
      }
      return absl::UnauthenticatedError("test signature mismatch");
    }
    return absl::NotFoundError("test key not found");
  }

 private:
  struct Key {
    std::string algorithm;
    std::string key_id;
    std::string secret;
  };
  std::vector<Key> keys_;
};

AuditCertificate BaseCertificate() {
  AuditCertificate certificate;
  certificate.checkpoint_manifest_hash =
      HashBytes(HashAlgorithm::kBlake3, "manifest");
  certificate.checkpoint_body_hash = HashBytes(HashAlgorithm::kBlake3, "body");
  certificate.tenant_id = "tenant-a";
  certificate.session_id = "session-1";
  certificate.branch_id = "main";
  certificate.event_range_start = 0;
  certificate.event_range_end = 3;
  certificate.log_generation = 3;
  certificate.schema_id = "incident_response_v1";
  certificate.model_artifact_hash = HashBytes(HashAlgorithm::kBlake3, "model");
  certificate.projection_model_id = "projection-model";
  certificate.auditor_model_id = "auditor";
  certificate.audit_policy_version = "exact-v1";
  certificate.verdict = AuditVerdict::kPass;
  certificate.drift_score = 0.0;
  certificate.provenance_root_hash = certificate.checkpoint_manifest_hash;
  certificate.created_unix_micros = 1777390000000000;
  return certificate;
}

TEST(AuditCertificateSignerTest,
     AddsAppendablePostQuantumSignatureWithoutChangingIdentity) {
  ASSERT_OK_AND_ASSIGN(AuditCertificate base,
                       FinalizeAuditCertificate(BaseCertificate()));
  TestOnlyMlDsaSigner signer(kAuditSignatureAlgorithmMlDsa65, "audit-key-1",
                             "secret");

  ASSERT_OK_AND_ASSIGN(AuditCertificate signed_certificate,
                       AddAuditCertificateSignature(base, signer));

  EXPECT_EQ(signed_certificate.certificate_id, base.certificate_id);
  ASSERT_EQ(signed_certificate.signatures.size(), 1);
  EXPECT_EQ(signed_certificate.signatures[0].algorithm,
            kAuditSignatureAlgorithmMlDsa65);
  EXPECT_EQ(signed_certificate.signatures[0].key_id, "audit-key-1");
}

TEST(AuditCertificateSignerTest, VerifiesAllowedSignatureAndRejectsTamper) {
  TestOnlyMlDsaSigner signer(kAuditSignatureAlgorithmMlDsa87, "audit-key-2",
                             "secret-2");
  TestOnlyMlDsaVerifier verifier;
  verifier.AddKey(kAuditSignatureAlgorithmMlDsa87, "audit-key-2", "secret-2");

  ASSERT_OK_AND_ASSIGN(AuditCertificate signed_certificate,
                       AddAuditCertificateSignature(BaseCertificate(), signer));
  ASSERT_OK_AND_ASSIGN(int valid,
                       CountValidAuditCertificateSignatures(
                           signed_certificate,
                           {kAuditSignatureAlgorithmMlDsa65,
                            kAuditSignatureAlgorithmMlDsa87},
                           verifier));
  EXPECT_EQ(valid, 1);

  AuditCertificate tampered = signed_certificate;
  tampered.drift_score = 0.25;
  ASSERT_OK_AND_ASSIGN(
      int tampered_valid,
      CountValidAuditCertificateSignatures(
          tampered,
          {kAuditSignatureAlgorithmMlDsa65, kAuditSignatureAlgorithmMlDsa87},
          verifier));
  EXPECT_EQ(tampered_valid, 0);
}

TEST(AuditCertificateSignerTest, SignedEncodingRoundTripsSignatures) {
  TestOnlyMlDsaSigner signer(kAuditSignatureAlgorithmMlDsa65, "audit-key-3",
                             "secret-3");
  ASSERT_OK_AND_ASSIGN(AuditCertificate signed_certificate,
                       AddAuditCertificateSignature(BaseCertificate(), signer));

  ASSERT_OK_AND_ASSIGN(std::string encoded,
                       EncodeSignedAuditCertificate(signed_certificate));
  ASSERT_OK_AND_ASSIGN(AuditCertificate decoded,
                       DecodeSignedAuditCertificate(encoded));

  EXPECT_EQ(decoded.certificate_id, signed_certificate.certificate_id);
  ASSERT_EQ(decoded.signatures.size(), 1);
  EXPECT_EQ(decoded.signatures[0], signed_certificate.signatures[0]);
}

TEST(AuditCertificateSignerTest, LocalLedgerPreservesSignatures) {
  LocalFilesystemAuditLedger ledger(TestRoot("signed_audit_ledger"));
  TestOnlyMlDsaSigner signer(kAuditSignatureAlgorithmMlDsa65, "audit-key-5",
                             "secret-5");
  ASSERT_OK_AND_ASSIGN(AuditCertificate signed_certificate,
                       AddAuditCertificateSignature(BaseCertificate(), signer));

  ASSERT_OK(ledger.PutCertificate(signed_certificate));
  ASSERT_OK_AND_ASSIGN(
      AuditCertificate stored,
      ledger.GetCertificate(signed_certificate.tenant_id,
                            signed_certificate.session_id,
                            signed_certificate.certificate_id));

  ASSERT_EQ(stored.signatures.size(), 1);
  EXPECT_EQ(stored.signatures[0], signed_certificate.signatures[0]);
}

TEST(AuditCertificateSignerTest, AlgorithmPolicyCanRequirePqSubset) {
  TestOnlyMlDsaSigner signer(kAuditSignatureAlgorithmMlDsa44, "audit-key-4",
                             "secret-4");
  TestOnlyMlDsaVerifier verifier;
  verifier.AddKey(kAuditSignatureAlgorithmMlDsa44, "audit-key-4", "secret-4");
  ASSERT_OK_AND_ASSIGN(AuditCertificate signed_certificate,
                       AddAuditCertificateSignature(BaseCertificate(), signer));

  ASSERT_OK_AND_ASSIGN(int valid,
                       CountValidAuditCertificateSignatures(
                           signed_certificate,
                           {kAuditSignatureAlgorithmMlDsa65,
                            kAuditSignatureAlgorithmMlDsa87},
                           verifier));
  EXPECT_EQ(valid, 0);
}

}  // namespace
}  // namespace litert::lm
