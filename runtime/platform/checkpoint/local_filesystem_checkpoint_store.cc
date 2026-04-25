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

#include "runtime/platform/checkpoint/local_filesystem_checkpoint_store.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/checkpoint/checkpoint_store.h"
#include "runtime/platform/checkpoint/durable_writer.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {
namespace {

constexpr std::array<char, 10> kPayloadMagic = {
    'D', 'P', 'M', 'P', 'A', 'Y', 'L', 'D', '1', '\n'};
constexpr std::array<char, 9> kManifestMagic = {
    'D', 'P', 'M', 'M', 'A', 'N', 'I', '1', '\n'};

void AppendU32(uint32_t v, std::string* out) {
  out->push_back(static_cast<char>(v & 0xff));
  out->push_back(static_cast<char>((v >> 8) & 0xff));
  out->push_back(static_cast<char>((v >> 16) & 0xff));
  out->push_back(static_cast<char>((v >> 24) & 0xff));
}

void AppendU64(uint64_t v, std::string* out) {
  for (int i = 0; i < 8; ++i)
    out->push_back(static_cast<char>((v >> (i * 8)) & 0xff));
}

absl::StatusOr<uint32_t> ReadU32(absl::string_view& view) {
  if (view.size() < 4) {
    return absl::DataLossError("checkpoint file truncated reading u32.");
  }
  uint32_t v =
      static_cast<uint32_t>(static_cast<unsigned char>(view[0])) |
      (static_cast<uint32_t>(static_cast<unsigned char>(view[1])) << 8) |
      (static_cast<uint32_t>(static_cast<unsigned char>(view[2])) << 16) |
      (static_cast<uint32_t>(static_cast<unsigned char>(view[3])) << 24);
  view.remove_prefix(4);
  return v;
}

absl::StatusOr<uint64_t> ReadU64(absl::string_view& view) {
  if (view.size() < 8) {
    return absl::DataLossError("checkpoint file truncated reading u64.");
  }
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(static_cast<unsigned char>(view[i]))
         << (i * 8);
  }
  view.remove_prefix(8);
  return v;
}

absl::Status ValidateIdentity(absl::string_view tenant_id,
                              absl::string_view session_id) {
  auto bad = [](absl::string_view v) {
    return v.empty() || v == "." || v == ".." ||
           v.find('/') != absl::string_view::npos ||
           v.find('\\') != absl::string_view::npos;
  };
  if (bad(tenant_id)) {
    return absl::InvalidArgumentError("checkpoint store: bad tenant_id.");
  }
  if (bad(session_id)) {
    return absl::InvalidArgumentError("checkpoint store: bad session_id.");
  }
  return absl::OkStatus();
}

}  // namespace

LocalFilesystemCheckpointStore::LocalFilesystemCheckpointStore(
    std::filesystem::path root_path)
    : root_path_(std::move(root_path)) {}

std::filesystem::path LocalFilesystemCheckpointStore::PayloadPathFor(
    absl::string_view tenant_id, absl::string_view session_id,
    const Hash256& body_hash) const {
  return root_path_ / std::string(tenant_id) / std::string(session_id) /
         "payloads" / (body_hash.ToHex() + ".dpmpayload");
}

std::filesystem::path LocalFilesystemCheckpointStore::ManifestPathFor(
    absl::string_view tenant_id, absl::string_view session_id,
    const Hash256& manifest_hash) const {
  return root_path_ / std::string(tenant_id) / std::string(session_id) /
         "manifests" / (manifest_hash.ToHex() + ".dpmmanifest");
}

// ----------------------------------------------------------------------
// Payloads.

absl::StatusOr<Hash256> LocalFilesystemCheckpointStore::PutPayload(
    absl::string_view tenant_id, absl::string_view session_id,
    absl::string_view payload_bytes, HashAlgorithm algo) {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  const Hash256 body_hash = HashBytes(algo, payload_bytes);
  const std::filesystem::path path =
      PayloadPathFor(tenant_id, session_id, body_hash);

  std::string framed;
  framed.reserve(kPayloadMagic.size() + 8 + payload_bytes.size());
  framed.append(kPayloadMagic.data(), kPayloadMagic.size());
  AppendU64(static_cast<uint64_t>(payload_bytes.size()), &framed);
  framed.append(payload_bytes.data(), payload_bytes.size());

  // Idempotent: same body_hash must mean same bytes. Mismatch is a
  // hash-collision or corruption signal.
  if (std::filesystem::exists(path)) {
    std::string existing;
    RETURN_IF_ERROR(ReadEntireFileIfExists(path, &existing));
    if (existing == framed) {
      return body_hash;
    }
    return absl::DataLossError(absl::StrCat(
        "checkpoint store: payload at ", body_hash.ToHex(),
        " already exists with different bytes; refusing to overwrite."));
  }
  RETURN_IF_ERROR(DurablyWriteFile(path, framed));
  return body_hash;
}

absl::StatusOr<std::string> LocalFilesystemCheckpointStore::GetPayload(
    absl::string_view tenant_id, absl::string_view session_id,
    const Hash256& body_hash) const {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  const std::filesystem::path path =
      PayloadPathFor(tenant_id, session_id, body_hash);
  if (!std::filesystem::exists(path)) {
    return absl::NotFoundError(absl::StrCat(
        "checkpoint payload not found: ", body_hash.ToHex()));
  }
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) {
    return absl::InternalError(absl::StrCat(
        "checkpoint store: failed to open ", path.string()));
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string data = buffer.str();
  absl::string_view view = data;
  if (view.size() < kPayloadMagic.size() ||
      std::memcmp(view.data(), kPayloadMagic.data(),
                  kPayloadMagic.size()) != 0) {
    return absl::DataLossError("payload missing magic header.");
  }
  view.remove_prefix(kPayloadMagic.size());
  ASSIGN_OR_RETURN(uint64_t size, ReadU64(view));
  if (view.size() < size) {
    return absl::DataLossError("payload truncated reading body.");
  }
  std::string out(view.data(), size);
  view.remove_prefix(size);
  if (!view.empty()) {
    return absl::DataLossError(absl::StrCat(
        "payload has ", view.size(), " trailing bytes."));
  }
  // Re-hash and compare against the requested address (BLAKE3 first then
  // SHA-256, mirroring the substrate's "stay proto-free" stance).
  const Hash256 b3 = HashBytes(HashAlgorithm::kBlake3, out);
  if (b3 == body_hash) return out;
  const Hash256 sha = HashBytes(HashAlgorithm::kSha256, out);
  if (sha == body_hash) return out;
  return absl::DataLossError(absl::StrCat(
      "payload hash mismatch: expected ", body_hash.ToHex(),
      ", got blake3=", b3.ToHex(), " sha256=", sha.ToHex()));
}

absl::StatusOr<bool> LocalFilesystemCheckpointStore::PayloadExists(
    absl::string_view tenant_id, absl::string_view session_id,
    const Hash256& body_hash) const {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  return std::filesystem::exists(
      PayloadPathFor(tenant_id, session_id, body_hash));
}

// ----------------------------------------------------------------------
// Manifests.

absl::Status LocalFilesystemCheckpointStore::PutManifest(
    absl::string_view tenant_id, absl::string_view session_id,
    const Hash256& manifest_hash, absl::string_view abi_bytes,
    const Hash256& referenced_body_hash) {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  if (abi_bytes.size() > 0xFFFFFFFFu) {
    return absl::ResourceExhaustedError(
        "checkpoint manifest ABI exceeds u32 size cap.");
  }

  const std::filesystem::path path =
      ManifestPathFor(tenant_id, session_id, manifest_hash);

  std::string framed;
  framed.reserve(kManifestMagic.size() + 4 + abi_bytes.size() + 32);
  framed.append(kManifestMagic.data(), kManifestMagic.size());
  AppendU32(static_cast<uint32_t>(abi_bytes.size()), &framed);
  framed.append(abi_bytes.data(), abi_bytes.size());
  framed.append(reinterpret_cast<const char*>(referenced_body_hash.bytes.data()),
                referenced_body_hash.bytes.size());

  if (std::filesystem::exists(path)) {
    std::string existing;
    RETURN_IF_ERROR(ReadEntireFileIfExists(path, &existing));
    if (existing == framed) {
      return absl::OkStatus();
    }
    return absl::DataLossError(absl::StrCat(
        "checkpoint store: manifest at ", manifest_hash.ToHex(),
        " already exists with different bytes; refusing to overwrite."));
  }
  return DurablyWriteFile(path, framed);
}

absl::StatusOr<CheckpointStore::ManifestRecord>
LocalFilesystemCheckpointStore::GetManifest(absl::string_view tenant_id,
                                            absl::string_view session_id,
                                            const Hash256& manifest_hash) const {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  const std::filesystem::path path =
      ManifestPathFor(tenant_id, session_id, manifest_hash);
  if (!std::filesystem::exists(path)) {
    return absl::NotFoundError(absl::StrCat(
        "manifest not found: ", manifest_hash.ToHex()));
  }
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) {
    return absl::InternalError(absl::StrCat(
        "checkpoint store: failed to open ", path.string()));
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string data = buffer.str();
  absl::string_view view = data;
  if (view.size() < kManifestMagic.size() ||
      std::memcmp(view.data(), kManifestMagic.data(),
                  kManifestMagic.size()) != 0) {
    return absl::DataLossError("manifest missing magic header.");
  }
  view.remove_prefix(kManifestMagic.size());
  ASSIGN_OR_RETURN(uint32_t abi_size, ReadU32(view));
  if (view.size() < abi_size) {
    return absl::DataLossError("manifest truncated reading ABI.");
  }
  ManifestRecord rec;
  rec.abi_bytes.assign(view.data(), abi_size);
  view.remove_prefix(abi_size);
  if (view.size() < rec.referenced_body_hash.bytes.size()) {
    return absl::DataLossError(
        "manifest truncated reading referenced_body_hash.");
  }
  std::memcpy(rec.referenced_body_hash.bytes.data(), view.data(),
              rec.referenced_body_hash.bytes.size());
  view.remove_prefix(rec.referenced_body_hash.bytes.size());
  if (!view.empty()) {
    return absl::DataLossError(absl::StrCat(
        "manifest has ", view.size(), " trailing bytes."));
  }
  return rec;
}

absl::StatusOr<bool> LocalFilesystemCheckpointStore::ManifestExists(
    absl::string_view tenant_id, absl::string_view session_id,
    const Hash256& manifest_hash) const {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  return std::filesystem::exists(
      ManifestPathFor(tenant_id, session_id, manifest_hash));
}

absl::StatusOr<std::vector<Hash256>>
LocalFilesystemCheckpointStore::ListManifests(
    absl::string_view tenant_id, absl::string_view session_id) const {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  const std::filesystem::path dir = root_path_ / std::string(tenant_id) /
                                    std::string(session_id) / "manifests";
  std::vector<Hash256> out;
  if (!std::filesystem::exists(dir)) return out;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    const std::string filename = entry.path().filename().string();
    constexpr absl::string_view kSuffix = ".dpmmanifest";
    if (filename.size() < kSuffix.size() + 64) continue;
    if (filename.compare(filename.size() - kSuffix.size(), kSuffix.size(),
                         kSuffix.data(), kSuffix.size()) != 0) {
      continue;
    }
    bool ok = false;
    Hash256 addr = Hash256::FromHex(
        absl::string_view(filename.data(),
                          filename.size() - kSuffix.size()),
        &ok);
    if (!ok) continue;
    out.push_back(addr);
  }
  return out;
}

}  // namespace litert::lm
