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

#include "runtime/platform/audit/audit_certificate.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/hash/hasher.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {
namespace {

bool IsZeroHash(const Hash256& hash) {
  static const Hash256 kZero;
  return hash == kZero;
}

void AppendU32(uint32_t v, std::string* out) {
  out->push_back(static_cast<char>(v & 0xff));
  out->push_back(static_cast<char>((v >> 8) & 0xff));
  out->push_back(static_cast<char>((v >> 16) & 0xff));
  out->push_back(static_cast<char>((v >> 24) & 0xff));
}

void AppendU64(uint64_t v, std::string* out) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<char>((v >> (i * 8)) & 0xff));
  }
}

void AppendI64(int64_t v, std::string* out) {
  AppendU64(static_cast<uint64_t>(v), out);
}

void AppendDouble(double v, std::string* out) {
  static_assert(sizeof(double) == sizeof(uint64_t));
  uint64_t u = 0;
  std::memcpy(&u, &v, sizeof(double));
  AppendU64(u, out);
}

absl::Status AppendString(absl::string_view v, std::string* out) {
  if (v.size() > std::numeric_limits<uint32_t>::max()) {
    return absl::ResourceExhaustedError("audit string exceeds u32 size cap.");
  }
  AppendU32(static_cast<uint32_t>(v.size()), out);
  out->append(v.data(), v.size());
  return absl::OkStatus();
}

absl::Status AppendHash(const Hash256& hash, std::string* out) {
  out->append(reinterpret_cast<const char*>(hash.bytes.data()),
              hash.bytes.size());
  return absl::OkStatus();
}

absl::Status AppendStringVector(const std::vector<std::string>& values,
                                std::string* out) {
  if (values.size() > std::numeric_limits<uint32_t>::max()) {
    return absl::ResourceExhaustedError("audit vector exceeds u32 size cap.");
  }
  AppendU32(static_cast<uint32_t>(values.size()), out);
  for (const std::string& v : values) {
    RETURN_IF_ERROR(AppendString(v, out));
  }
  return absl::OkStatus();
}

absl::Status AppendSignatureVector(
    const std::vector<AuditCertificateSignature>& signatures,
    std::string* out) {
  if (signatures.size() > std::numeric_limits<uint32_t>::max()) {
    return absl::ResourceExhaustedError(
        "audit signature vector exceeds u32 size cap.");
  }
  AppendU32(static_cast<uint32_t>(signatures.size()), out);
  for (const AuditCertificateSignature& signature : signatures) {
    RETURN_IF_ERROR(AppendString(signature.algorithm, out));
    RETURN_IF_ERROR(AppendString(signature.key_id, out));
    RETURN_IF_ERROR(AppendString(signature.signature, out));
  }
  return absl::OkStatus();
}

absl::StatusOr<uint32_t> ReadU32(absl::string_view* view) {
  if (view->size() < 4) return absl::DataLossError("audit truncated u32.");
  const unsigned char* p =
      reinterpret_cast<const unsigned char*>(view->data());
  uint32_t v = static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
  view->remove_prefix(4);
  return v;
}

absl::StatusOr<uint64_t> ReadU64(absl::string_view* view) {
  if (view->size() < 8) return absl::DataLossError("audit truncated u64.");
  const unsigned char* p =
      reinterpret_cast<const unsigned char*>(view->data());
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (i * 8);
  view->remove_prefix(8);
  return v;
}

absl::StatusOr<int64_t> ReadI64(absl::string_view* view) {
  ASSIGN_OR_RETURN(uint64_t v, ReadU64(view));
  return static_cast<int64_t>(v);
}

absl::StatusOr<double> ReadDouble(absl::string_view* view) {
  ASSIGN_OR_RETURN(uint64_t u, ReadU64(view));
  double v = 0.0;
  std::memcpy(&v, &u, sizeof(double));
  return v;
}

absl::StatusOr<std::string> ReadString(absl::string_view* view) {
  ASSIGN_OR_RETURN(uint32_t size, ReadU32(view));
  if (view->size() < size) return absl::DataLossError("audit truncated str.");
  std::string out(view->data(), size);
  view->remove_prefix(size);
  return out;
}

absl::StatusOr<Hash256> ReadHash(absl::string_view* view) {
  if (view->size() < 32) return absl::DataLossError("audit truncated hash.");
  Hash256 hash;
  std::memcpy(hash.bytes.data(), view->data(), hash.bytes.size());
  view->remove_prefix(hash.bytes.size());
  return hash;
}

absl::StatusOr<std::vector<std::string>> ReadStringVector(
    absl::string_view* view) {
  ASSIGN_OR_RETURN(uint32_t count, ReadU32(view));
  std::vector<std::string> out;
  out.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    ASSIGN_OR_RETURN(std::string v, ReadString(view));
    out.push_back(std::move(v));
  }
  return out;
}

absl::StatusOr<std::vector<AuditCertificateSignature>> ReadSignatureVector(
    absl::string_view* view) {
  ASSIGN_OR_RETURN(uint32_t count, ReadU32(view));
  std::vector<AuditCertificateSignature> out;
  out.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    AuditCertificateSignature signature;
    ASSIGN_OR_RETURN(signature.algorithm, ReadString(view));
    ASSIGN_OR_RETURN(signature.key_id, ReadString(view));
    ASSIGN_OR_RETURN(signature.signature, ReadString(view));
    out.push_back(std::move(signature));
  }
  return out;
}

absl::Status ValidateSignatureForStorage(
    const AuditCertificateSignature& signature) {
  if (signature.algorithm.empty() || signature.key_id.empty() ||
      signature.signature.empty()) {
    return absl::InvalidArgumentError(
        "AuditCertificate signatures require algorithm, key_id, and bytes.");
  }
  return absl::OkStatus();
}

}  // namespace

absl::string_view AuditVerdictToString(AuditVerdict verdict) {
  switch (verdict) {
    case AuditVerdict::kPending:
      return "pending";
    case AuditVerdict::kPass:
      return "pass";
    case AuditVerdict::kCorrectionEmitted:
      return "correction_emitted";
    case AuditVerdict::kInconclusive:
      return "inconclusive";
  }
  return "inconclusive";
}

absl::StatusOr<AuditVerdict> AuditVerdictFromString(
    absl::string_view verdict) {
  if (verdict == "pending") return AuditVerdict::kPending;
  if (verdict == "pass") return AuditVerdict::kPass;
  if (verdict == "correction_emitted") {
    return AuditVerdict::kCorrectionEmitted;
  }
  if (verdict == "inconclusive") return AuditVerdict::kInconclusive;
  return absl::InvalidArgumentError(
      absl::StrCat("unknown audit verdict: ", verdict));
}

absl::Status ValidateAuditCertificateForHashing(
    const AuditCertificate& certificate) {
  if (IsZeroHash(certificate.checkpoint_manifest_hash)) {
    return absl::InvalidArgumentError(
        "AuditCertificate requires checkpoint_manifest_hash.");
  }
  if (IsZeroHash(certificate.checkpoint_body_hash)) {
    return absl::InvalidArgumentError(
        "AuditCertificate requires checkpoint_body_hash.");
  }
  if (certificate.tenant_id.empty() || certificate.session_id.empty() ||
      certificate.branch_id.empty()) {
    return absl::InvalidArgumentError(
        "AuditCertificate requires tenant/session/branch identity.");
  }
  if (certificate.event_range_end < certificate.event_range_start) {
    return absl::InvalidArgumentError(
        "AuditCertificate event range is inverted.");
  }
  if (certificate.schema_id.empty() ||
      IsZeroHash(certificate.model_artifact_hash) ||
      certificate.projection_model_id.empty() ||
      certificate.auditor_model_id.empty() ||
      certificate.audit_policy_version.empty()) {
    return absl::InvalidArgumentError(
        "AuditCertificate requires schema/model/auditor/policy binding.");
  }
  if (std::isnan(certificate.drift_score) ||
      certificate.drift_score < 0.0 || certificate.drift_score > 1.0) {
    return absl::InvalidArgumentError(
        "AuditCertificate drift_score must be in [0.0, 1.0].");
  }
  if (certificate.created_unix_micros <= 0) {
    return absl::InvalidArgumentError(
        "AuditCertificate requires created_unix_micros.");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> EncodeCanonicalAuditCertificate(
    const AuditCertificate& certificate) {
  RETURN_IF_ERROR(ValidateAuditCertificateForHashing(certificate));
  std::string out;
  AppendU32(kAuditCertificateVersion, &out);
  RETURN_IF_ERROR(AppendHash(certificate.checkpoint_manifest_hash, &out));
  RETURN_IF_ERROR(AppendHash(certificate.checkpoint_body_hash, &out));
  RETURN_IF_ERROR(AppendString(certificate.tenant_id, &out));
  RETURN_IF_ERROR(AppendString(certificate.session_id, &out));
  RETURN_IF_ERROR(AppendString(certificate.branch_id, &out));
  AppendU64(certificate.event_range_start, &out);
  AppendU64(certificate.event_range_end, &out);
  AppendU64(certificate.log_generation, &out);
  RETURN_IF_ERROR(AppendString(certificate.schema_id, &out));
  RETURN_IF_ERROR(AppendHash(certificate.model_artifact_hash, &out));
  RETURN_IF_ERROR(AppendString(certificate.projection_model_id, &out));
  RETURN_IF_ERROR(AppendString(certificate.auditor_model_id, &out));
  RETURN_IF_ERROR(AppendString(certificate.audit_policy_version, &out));
  RETURN_IF_ERROR(
      AppendString(AuditVerdictToString(certificate.verdict), &out));
  AppendDouble(certificate.drift_score, &out);
  RETURN_IF_ERROR(AppendStringVector(certificate.drift_fields, &out));
  RETURN_IF_ERROR(AppendStringVector(certificate.correction_event_ids, &out));
  RETURN_IF_ERROR(AppendHash(certificate.provenance_root_hash, &out));
  AppendI64(certificate.created_unix_micros, &out);
  AppendI64(certificate.expires_unix_micros, &out);
  return out;
}

absl::StatusOr<AuditCertificate> DecodeCanonicalAuditCertificate(
    absl::string_view bytes) {
  absl::string_view view = bytes;
  ASSIGN_OR_RETURN(uint32_t version, ReadU32(&view));
  if (version != kAuditCertificateVersion) {
    return absl::DataLossError("unsupported audit certificate version.");
  }
  AuditCertificate certificate;
  ASSIGN_OR_RETURN(certificate.checkpoint_manifest_hash, ReadHash(&view));
  ASSIGN_OR_RETURN(certificate.checkpoint_body_hash, ReadHash(&view));
  ASSIGN_OR_RETURN(certificate.tenant_id, ReadString(&view));
  ASSIGN_OR_RETURN(certificate.session_id, ReadString(&view));
  ASSIGN_OR_RETURN(certificate.branch_id, ReadString(&view));
  ASSIGN_OR_RETURN(certificate.event_range_start, ReadU64(&view));
  ASSIGN_OR_RETURN(certificate.event_range_end, ReadU64(&view));
  ASSIGN_OR_RETURN(certificate.log_generation, ReadU64(&view));
  ASSIGN_OR_RETURN(certificate.schema_id, ReadString(&view));
  ASSIGN_OR_RETURN(certificate.model_artifact_hash, ReadHash(&view));
  ASSIGN_OR_RETURN(certificate.projection_model_id, ReadString(&view));
  ASSIGN_OR_RETURN(certificate.auditor_model_id, ReadString(&view));
  ASSIGN_OR_RETURN(certificate.audit_policy_version, ReadString(&view));
  ASSIGN_OR_RETURN(std::string verdict, ReadString(&view));
  ASSIGN_OR_RETURN(certificate.verdict, AuditVerdictFromString(verdict));
  ASSIGN_OR_RETURN(certificate.drift_score, ReadDouble(&view));
  ASSIGN_OR_RETURN(certificate.drift_fields, ReadStringVector(&view));
  ASSIGN_OR_RETURN(certificate.correction_event_ids, ReadStringVector(&view));
  ASSIGN_OR_RETURN(certificate.provenance_root_hash, ReadHash(&view));
  ASSIGN_OR_RETURN(certificate.created_unix_micros, ReadI64(&view));
  ASSIGN_OR_RETURN(certificate.expires_unix_micros, ReadI64(&view));
  if (!view.empty()) {
    return absl::DataLossError("audit certificate trailing bytes.");
  }
  RETURN_IF_ERROR(ValidateAuditCertificateForHashing(certificate));
  ASSIGN_OR_RETURN(certificate.certificate_id,
                   ComputeAuditCertificateId(certificate));
  return certificate;
}

absl::StatusOr<std::string> EncodeSignedAuditCertificate(
    const AuditCertificate& certificate) {
  ASSIGN_OR_RETURN(AuditCertificate finalized,
                   FinalizeAuditCertificate(certificate));
  ASSIGN_OR_RETURN(std::string canonical,
                   EncodeCanonicalAuditCertificate(finalized));
  std::string out;
  AppendU32(kAuditCertificateVersion, &out);
  RETURN_IF_ERROR(AppendString(canonical, &out));
  for (const AuditCertificateSignature& signature : finalized.signatures) {
    RETURN_IF_ERROR(ValidateSignatureForStorage(signature));
  }
  RETURN_IF_ERROR(AppendSignatureVector(finalized.signatures, &out));
  return out;
}

absl::StatusOr<AuditCertificate> DecodeSignedAuditCertificate(
    absl::string_view bytes) {
  absl::string_view view = bytes;
  ASSIGN_OR_RETURN(uint32_t version, ReadU32(&view));
  if (version != kAuditCertificateVersion) {
    return absl::DataLossError("unsupported signed audit certificate version.");
  }
  ASSIGN_OR_RETURN(std::string canonical, ReadString(&view));
  ASSIGN_OR_RETURN(AuditCertificate certificate,
                   DecodeCanonicalAuditCertificate(canonical));
  ASSIGN_OR_RETURN(certificate.signatures, ReadSignatureVector(&view));
  if (!view.empty()) {
    return absl::DataLossError("signed audit certificate trailing bytes.");
  }
  for (const AuditCertificateSignature& signature : certificate.signatures) {
    RETURN_IF_ERROR(ValidateSignatureForStorage(signature));
  }
  return certificate;
}

absl::StatusOr<Hash256> ComputeAuditCertificateId(
    const AuditCertificate& certificate) {
  ASSIGN_OR_RETURN(std::string canonical,
                   EncodeCanonicalAuditCertificate(certificate));
  return HashBytes(HashAlgorithm::kBlake3, canonical);
}

absl::StatusOr<AuditCertificate> FinalizeAuditCertificate(
    AuditCertificate certificate) {
  ASSIGN_OR_RETURN(certificate.certificate_id,
                   ComputeAuditCertificateId(certificate));
  return certificate;
}

}  // namespace litert::lm
