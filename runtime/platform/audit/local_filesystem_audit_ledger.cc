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

#include <array>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/str_split.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/audit/audit_certificate.h"
#include "runtime/platform/checkpoint/durable_writer.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {
namespace {

constexpr std::array<char, 10> kAuditMagic = {'D', 'P', 'M', 'A', 'U',
                                              'D', 'I', 'T', '1', '\n'};
constexpr absl::string_view kLatestIndexMagic = "DPMAUDIT-LATEST-1";

struct LatestIndexRecord {
  Hash256 checkpoint_manifest_hash;
  Hash256 certificate_id;
  int64_t created_unix_micros = 0;
};

absl::Status ValidateIdentity(absl::string_view tenant_id,
                              absl::string_view session_id) {
  auto bad = [](absl::string_view v) {
    return v.empty() || v == "." || v == ".." ||
           v.find('/') != absl::string_view::npos ||
           v.find('\\') != absl::string_view::npos;
  };
  if (bad(tenant_id) || bad(session_id)) {
    return absl::InvalidArgumentError("audit ledger: bad identity.");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) {
    return absl::InternalError(
        absl::StrCat("audit ledger: failed to open ", path.string()));
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

absl::StatusOr<AuditCertificate> DecodeFramed(absl::string_view data) {
  if (data.size() < kAuditMagic.size() ||
      std::memcmp(data.data(), kAuditMagic.data(), kAuditMagic.size()) != 0) {
    return absl::DataLossError("audit certificate missing magic header.");
  }
  data.remove_prefix(kAuditMagic.size());
  absl::StatusOr<AuditCertificate> signed_certificate =
      DecodeSignedAuditCertificate(data);
  if (signed_certificate.ok()) return signed_certificate;
  return DecodeCanonicalAuditCertificate(data);
}

bool IsZeroHash(const Hash256& hash) {
  static const Hash256 kZero;
  return hash == kZero;
}

std::string EncodeLatestIndex(const AuditCertificate& certificate) {
  return absl::StrCat(
      kLatestIndexMagic, "\n",
      certificate.checkpoint_manifest_hash.ToHex(), "\n",
      certificate.certificate_id.ToHex(), "\n",
      certificate.created_unix_micros, "\n");
}

absl::StatusOr<LatestIndexRecord> DecodeLatestIndex(absl::string_view data) {
  std::vector<std::string> lines = absl::StrSplit(data, '\n');
  if (lines.size() < 4 || lines[0] != kLatestIndexMagic) {
    return absl::DataLossError("audit latest index missing magic header.");
  }
  bool ok = false;
  LatestIndexRecord record;
  record.checkpoint_manifest_hash = Hash256::FromHex(lines[1], &ok);
  if (!ok) {
    return absl::DataLossError(
        "audit latest index has invalid checkpoint hash.");
  }
  record.certificate_id = Hash256::FromHex(lines[2], &ok);
  if (!ok) {
    return absl::DataLossError(
        "audit latest index has invalid certificate hash.");
  }
  try {
    size_t parsed = 0;
    record.created_unix_micros = std::stoll(lines[3], &parsed);
    if (parsed != lines[3].size()) {
      return absl::DataLossError(
          "audit latest index has invalid created_unix_micros.");
    }
  } catch (const std::exception&) {
    return absl::DataLossError(
        "audit latest index has invalid created_unix_micros.");
  }
  return record;
}

}  // namespace

LocalFilesystemAuditLedger::LocalFilesystemAuditLedger(
    std::filesystem::path root_path)
    : root_path_(std::move(root_path)) {}

std::filesystem::path LocalFilesystemAuditLedger::CertificatePathFor(
    absl::string_view tenant_id, absl::string_view session_id,
    const Hash256& certificate_id) const {
  return root_path_ / std::string(tenant_id) / std::string(session_id) /
         "certificates" / (certificate_id.ToHex() + ".dpmaudit");
}

std::filesystem::path LocalFilesystemAuditLedger::LatestIndexPathFor(
    absl::string_view tenant_id, absl::string_view session_id,
    const Hash256& checkpoint_manifest_hash) const {
  return root_path_ / std::string(tenant_id) / std::string(session_id) /
         "certificates_by_checkpoint" /
         (checkpoint_manifest_hash.ToHex() + ".latest");
}

absl::Status LocalFilesystemAuditLedger::PutCertificate(
    const AuditCertificate& certificate) {
  RETURN_IF_ERROR(ValidateIdentity(certificate.tenant_id,
                                   certificate.session_id));
  ASSIGN_OR_RETURN(AuditCertificate finalized,
                   FinalizeAuditCertificate(certificate));
  if (!IsZeroHash(certificate.certificate_id) &&
      certificate.certificate_id != finalized.certificate_id) {
    return absl::InvalidArgumentError(
        "audit certificate_id does not match canonical bytes.");
  }
  ASSIGN_OR_RETURN(std::string encoded,
                   EncodeSignedAuditCertificate(finalized));
  std::string framed;
  framed.reserve(kAuditMagic.size() + encoded.size());
  framed.append(kAuditMagic.data(), kAuditMagic.size());
  framed.append(encoded);

  const std::filesystem::path path = CertificatePathFor(
      finalized.tenant_id, finalized.session_id, finalized.certificate_id);
  if (std::filesystem::exists(path)) {
    ASSIGN_OR_RETURN(std::string existing, ReadFile(path));
    if (existing == framed) return absl::OkStatus();
    return absl::DataLossError(absl::StrCat(
        "audit certificate ", finalized.certificate_id.ToHex(),
        " already exists with different bytes."));
  }
  RETURN_IF_ERROR(DurablyWriteFile(path, framed));

  const std::filesystem::path index_path =
      LatestIndexPathFor(finalized.tenant_id, finalized.session_id,
                         finalized.checkpoint_manifest_hash);
  std::string existing_index;
  RETURN_IF_ERROR(ReadEntireFileIfExists(index_path, &existing_index));
  if (!existing_index.empty()) {
    absl::StatusOr<LatestIndexRecord> existing =
        DecodeLatestIndex(existing_index);
    if (existing.ok() &&
        existing->checkpoint_manifest_hash ==
            finalized.checkpoint_manifest_hash &&
        existing->created_unix_micros >= finalized.created_unix_micros) {
      return absl::OkStatus();
    }
  }
  return DurablyWriteFile(index_path, EncodeLatestIndex(finalized));
}

absl::StatusOr<AuditCertificate> LocalFilesystemAuditLedger::GetCertificate(
    absl::string_view tenant_id, absl::string_view session_id,
    const Hash256& certificate_id) const {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  const std::filesystem::path path =
      CertificatePathFor(tenant_id, session_id, certificate_id);
  if (!std::filesystem::exists(path)) {
    return absl::NotFoundError(
        absl::StrCat("audit certificate not found: ",
                     certificate_id.ToHex()));
  }
  ASSIGN_OR_RETURN(std::string data, ReadFile(path));
  ASSIGN_OR_RETURN(AuditCertificate certificate, DecodeFramed(data));
  if (certificate.certificate_id != certificate_id) {
    return absl::DataLossError("audit certificate path/id mismatch.");
  }
  return certificate;
}

absl::StatusOr<std::vector<AuditCertificate>>
LocalFilesystemAuditLedger::ListForSession(
    absl::string_view tenant_id, absl::string_view session_id) const {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  const std::filesystem::path dir =
      root_path_ / std::string(tenant_id) / std::string(session_id) /
      "certificates";
  std::vector<AuditCertificate> out;
  if (!std::filesystem::exists(dir)) return out;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    ASSIGN_OR_RETURN(std::string data, ReadFile(entry.path()));
    ASSIGN_OR_RETURN(AuditCertificate certificate, DecodeFramed(data));
    out.push_back(std::move(certificate));
  }
  return out;
}

absl::StatusOr<std::vector<AuditCertificate>>
LocalFilesystemAuditLedger::ListForCheckpoint(
    absl::string_view tenant_id, absl::string_view session_id,
    const Hash256& checkpoint_manifest_hash) const {
  ASSIGN_OR_RETURN(std::vector<AuditCertificate> certificates,
                   ListForSession(tenant_id, session_id));
  std::vector<AuditCertificate> out;
  for (AuditCertificate& certificate : certificates) {
    if (certificate.checkpoint_manifest_hash == checkpoint_manifest_hash) {
      out.push_back(std::move(certificate));
    }
  }
  return out;
}

absl::StatusOr<AuditCertificate>
LocalFilesystemAuditLedger::LatestForCheckpoint(
    absl::string_view tenant_id, absl::string_view session_id,
    const Hash256& checkpoint_manifest_hash) const {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  std::string index_bytes;
  RETURN_IF_ERROR(ReadEntireFileIfExists(
      LatestIndexPathFor(tenant_id, session_id, checkpoint_manifest_hash),
      &index_bytes));
  if (!index_bytes.empty()) {
    absl::StatusOr<LatestIndexRecord> index = DecodeLatestIndex(index_bytes);
    if (index.ok() &&
        index->checkpoint_manifest_hash == checkpoint_manifest_hash) {
      absl::StatusOr<AuditCertificate> indexed =
          GetCertificate(tenant_id, session_id, index->certificate_id);
      if (indexed.ok() &&
          indexed->checkpoint_manifest_hash == checkpoint_manifest_hash) {
        return indexed;
      }
    }
  }

  ASSIGN_OR_RETURN(std::vector<AuditCertificate> certificates,
                   ListForCheckpoint(tenant_id, session_id,
                                     checkpoint_manifest_hash));
  if (certificates.empty()) {
    return absl::NotFoundError("audit certificate not found for checkpoint.");
  }
  AuditCertificate latest = certificates.front();
  for (const AuditCertificate& certificate : certificates) {
    if (certificate.created_unix_micros > latest.created_unix_micros) {
      latest = certificate;
    }
  }
  return latest;
}

}  // namespace litert::lm
