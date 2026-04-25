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

constexpr std::array<char, 10> kStoreMagic = {'D', 'P', 'M', 'S', 'T',
                                              'O', 'R', 'E', '1', '\n'};

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

absl::StatusOr<uint32_t> ReadU32(absl::string_view& view) {
  if (view.size() < 4) {
    return absl::DataLossError(".dpmckpt truncated reading u32.");
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
    return absl::DataLossError(".dpmckpt truncated reading u64.");
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

std::filesystem::path LocalFilesystemCheckpointStore::PathFor(
    absl::string_view tenant_id, absl::string_view session_id,
    const Hash256& address) const {
  return root_path_ / std::string(tenant_id) / std::string(session_id) /
         (address.ToHex() + ".dpmckpt");
}

absl::StatusOr<Hash256> LocalFilesystemCheckpointStore::Put(
    absl::string_view tenant_id, absl::string_view session_id,
    absl::string_view abi_bytes, absl::string_view payload_bytes,
    HashAlgorithm algo) {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  if (abi_bytes.size() > 0xFFFFFFFFu) {
    return absl::ResourceExhaustedError(
        "checkpoint ABI exceeds u32 size cap.");
  }

  const Hash256 address = HashBytes(algo, payload_bytes);
  const std::filesystem::path path = PathFor(tenant_id, session_id, address);

  std::string framed;
  framed.reserve(kStoreMagic.size() + 4 + abi_bytes.size() + 8 +
                 payload_bytes.size());
  framed.append(kStoreMagic.data(), kStoreMagic.size());
  AppendU32(static_cast<uint32_t>(abi_bytes.size()), &framed);
  framed.append(abi_bytes.data(), abi_bytes.size());
  AppendU64(static_cast<uint64_t>(payload_bytes.size()), &framed);
  framed.append(payload_bytes.data(), payload_bytes.size());

  // Idempotent Put. The address is content-derived from payload_bytes, but
  // an earlier writer may have stored the same payload under a *different*
  // ABI; we must verify the bytes-on-disk match before declaring no-op.
  // (Otherwise a torn write or a hash collision on payload_bytes alone
  // would silently mask a manifest mismatch.)
  if (std::filesystem::exists(path)) {
    std::string existing;
    RETURN_IF_ERROR(ReadEntireFileIfExists(path, &existing));
    if (existing == framed) {
      return address;
    }
    return absl::DataLossError(absl::StrCat(
        "checkpoint store: address ", address.ToHex(),
        " already exists with different bytes; refusing to overwrite. "
        "Identical payload_bytes under a different ABI indicates a "
        "manifest collision."));
  }

  // Durable write: temp file -> fsync -> atomic rename -> dir fsync.
  RETURN_IF_ERROR(DurablyWriteFile(path, framed));
  return address;
}

absl::StatusOr<CheckpointStore::CheckpointBlob>
LocalFilesystemCheckpointStore::Get(absl::string_view tenant_id,
                                    absl::string_view session_id,
                                    const Hash256& address) const {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  const std::filesystem::path path = PathFor(tenant_id, session_id, address);
  if (!std::filesystem::exists(path)) {
    return absl::NotFoundError(
        absl::StrCat("checkpoint not found: ", address.ToHex()));
  }
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) {
    return absl::InternalError(
        absl::StrCat("checkpoint store: failed to open ", path.string()));
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string data = buffer.str();
  absl::string_view view = data;
  if (view.size() < kStoreMagic.size() ||
      std::memcmp(view.data(), kStoreMagic.data(), kStoreMagic.size()) != 0) {
    return absl::DataLossError("checkpoint file missing magic header.");
  }
  view.remove_prefix(kStoreMagic.size());
  ASSIGN_OR_RETURN(uint32_t abi_size, ReadU32(view));
  if (view.size() < abi_size) {
    return absl::DataLossError("checkpoint file truncated reading ABI.");
  }
  CheckpointBlob blob;
  blob.abi_bytes.assign(view.data(), abi_size);
  view.remove_prefix(abi_size);
  ASSIGN_OR_RETURN(uint64_t payload_size, ReadU64(view));
  if (view.size() < payload_size) {
    return absl::DataLossError("checkpoint file truncated reading payload.");
  }
  blob.payload_bytes.assign(view.data(), payload_size);
  view.remove_prefix(payload_size);
  if (!view.empty()) {
    return absl::DataLossError(absl::StrCat(
        "checkpoint file has ", view.size(), " trailing bytes."));
  }

  // Recompute the hash and verify against the requested address. Try
  // BLAKE3 first; on mismatch try SHA-256 (the algorithm is recorded in
  // the ABI bytes at the proto level, but the substrate cannot decode it
  // without linking the proto. Trying both costs one extra hash on the
  // FIPS path and keeps the substrate proto-free).
  const Hash256 b3 = HashBytes(HashAlgorithm::kBlake3, blob.payload_bytes);
  if (b3 == address) {
    blob.algorithm = HashAlgorithm::kBlake3;
    return blob;
  }
  const Hash256 sha = HashBytes(HashAlgorithm::kSha256, blob.payload_bytes);
  if (sha == address) {
    blob.algorithm = HashAlgorithm::kSha256;
    return blob;
  }
  return absl::DataLossError(absl::StrCat(
      "checkpoint payload hash mismatch: expected ", address.ToHex(),
      ", got blake3=", b3.ToHex(), " sha256=", sha.ToHex()));
}

absl::StatusOr<bool> LocalFilesystemCheckpointStore::Exists(
    absl::string_view tenant_id, absl::string_view session_id,
    const Hash256& address) const {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  return std::filesystem::exists(PathFor(tenant_id, session_id, address));
}

absl::StatusOr<std::vector<Hash256>> LocalFilesystemCheckpointStore::List(
    absl::string_view tenant_id, absl::string_view session_id) const {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  const std::filesystem::path dir =
      root_path_ / std::string(tenant_id) / std::string(session_id);
  std::vector<Hash256> out;
  if (!std::filesystem::exists(dir)) return out;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    const std::string filename = entry.path().filename().string();
    constexpr absl::string_view kSuffix = ".dpmckpt";
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
