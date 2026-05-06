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

#include "runtime/platform/provenance/local_merkle_dag_store.h"

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
#include "runtime/platform/checkpoint/durable_writer.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/platform/provenance/merkle_dag_store.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {
namespace {

constexpr std::array<char, 9> kDagMagic = {'D', 'P', 'M', 'D', 'A',
                                           'G', '1', '\n', '\0'};

void AppendU32(uint32_t v, std::string* out) {
  out->push_back(static_cast<char>(v & 0xff));
  out->push_back(static_cast<char>((v >> 8) & 0xff));
  out->push_back(static_cast<char>((v >> 16) & 0xff));
  out->push_back(static_cast<char>((v >> 24) & 0xff));
}

void AppendI64(int64_t v, std::string* out) {
  uint64_t u = static_cast<uint64_t>(v);
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<char>((u >> (i * 8)) & 0xff));
  }
}

absl::StatusOr<uint32_t> ReadU32(absl::string_view& view) {
  if (view.size() < 4) return absl::DataLossError("dagnode truncated u32.");
  uint32_t v =
      static_cast<uint32_t>(static_cast<unsigned char>(view[0])) |
      (static_cast<uint32_t>(static_cast<unsigned char>(view[1])) << 8) |
      (static_cast<uint32_t>(static_cast<unsigned char>(view[2])) << 16) |
      (static_cast<uint32_t>(static_cast<unsigned char>(view[3])) << 24);
  view.remove_prefix(4);
  return v;
}

absl::StatusOr<int64_t> ReadI64(absl::string_view& view) {
  if (view.size() < 8) return absl::DataLossError("dagnode truncated i64.");
  uint64_t u = 0;
  for (int i = 0; i < 8; ++i) {
    u |= static_cast<uint64_t>(static_cast<unsigned char>(view[i]))
         << (i * 8);
  }
  view.remove_prefix(8);
  return static_cast<int64_t>(u);
}

absl::Status ValidateIdentity(absl::string_view tenant_id,
                              absl::string_view session_id) {
  auto bad = [](absl::string_view v) {
    return v.empty() || v == "." || v == ".." ||
           v.find('/') != absl::string_view::npos ||
           v.find('\\') != absl::string_view::npos;
  };
  if (bad(tenant_id) || bad(session_id)) {
    return absl::InvalidArgumentError("dag store: bad identity.");
  }
  return absl::OkStatus();
}

}  // namespace

LocalMerkleDagStore::LocalMerkleDagStore(std::filesystem::path root_path)
    : root_path_(std::move(root_path)) {}

std::filesystem::path LocalMerkleDagStore::PathFor(
    absl::string_view tenant_id, absl::string_view session_id,
    const Hash256& hash) const {
  return root_path_ / std::string(tenant_id) / std::string(session_id) /
         "dag" / (hash.ToHex() + ".dagnode");
}

absl::Status LocalMerkleDagStore::Put(absl::string_view tenant_id,
                                      absl::string_view session_id,
                                      const MerkleDagNode& node) {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  if (node.parent_hashes.size() > 0xFFFFu) {
    return absl::ResourceExhaustedError(
        "dag node has too many parents (>65535).");
  }
  if (node.annotations.size() > 0xFFFFFFFFu) {
    return absl::ResourceExhaustedError("dag node annotations exceed u32.");
  }

  std::string body;
  body.reserve(kDagMagic.size() + 32 + 4 + node.parent_hashes.size() * 32 +
               8 + 4 + node.annotations.size());
  body.append(kDagMagic.data(), kDagMagic.size());
  body.append(reinterpret_cast<const char*>(node.hash.bytes.data()),
              node.hash.bytes.size());
  AppendU32(static_cast<uint32_t>(node.parent_hashes.size()), &body);
  for (const Hash256& p : node.parent_hashes) {
    body.append(reinterpret_cast<const char*>(p.bytes.data()),
                p.bytes.size());
  }
  AppendI64(node.created_unix_micros, &body);
  AppendU32(static_cast<uint32_t>(node.annotations.size()), &body);
  body.append(node.annotations);

  const std::filesystem::path path = PathFor(tenant_id, session_id, node.hash);

  // Idempotent Put with content verification: the on-disk node must match
  // the bytes we are about to write. A "same hash, same content" claim
  // without a content check would mask any earlier torn-write artifact or
  // off-line mutation.
  if (std::filesystem::exists(path)) {
    std::string existing;
    RETURN_IF_ERROR(ReadEntireFileIfExists(path, &existing));
    if (existing == body) {
      return absl::OkStatus();
    }
    return absl::DataLossError(absl::StrCat(
        "dag store: node ", node.hash.ToHex(),
        " already exists with different bytes; refusing to overwrite."));
  }
  return DurablyWriteFile(path, body);
}

absl::StatusOr<MerkleDagNode> LocalMerkleDagStore::Get(
    absl::string_view tenant_id, absl::string_view session_id,
    const Hash256& hash) const {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  const std::filesystem::path path = PathFor(tenant_id, session_id, hash);
  if (!std::filesystem::exists(path)) {
    return absl::NotFoundError(
        absl::StrCat("dag node not found: ", hash.ToHex()));
  }
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) {
    return absl::InternalError(
        absl::StrCat("dag store: failed to open ", path.string()));
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string data = buffer.str();
  absl::string_view view = data;
  if (view.size() < kDagMagic.size() ||
      std::memcmp(view.data(), kDagMagic.data(), kDagMagic.size()) != 0) {
    return absl::DataLossError("dagnode missing magic header.");
  }
  view.remove_prefix(kDagMagic.size());

  MerkleDagNode node;
  if (view.size() < node.hash.bytes.size()) {
    return absl::DataLossError("dagnode truncated reading own hash.");
  }
  std::memcpy(node.hash.bytes.data(), view.data(), node.hash.bytes.size());
  view.remove_prefix(node.hash.bytes.size());

  ASSIGN_OR_RETURN(uint32_t parent_count, ReadU32(view));
  for (uint32_t i = 0; i < parent_count; ++i) {
    if (view.size() < 32) {
      return absl::DataLossError("dagnode truncated reading parent hash.");
    }
    Hash256 p;
    std::memcpy(p.bytes.data(), view.data(), 32);
    view.remove_prefix(32);
    node.parent_hashes.push_back(p);
  }
  ASSIGN_OR_RETURN(int64_t created, ReadI64(view));
  node.created_unix_micros = created;
  ASSIGN_OR_RETURN(uint32_t ann_size, ReadU32(view));
  if (view.size() < ann_size) {
    return absl::DataLossError("dagnode truncated reading annotations.");
  }
  node.annotations.assign(view.data(), ann_size);
  view.remove_prefix(ann_size);
  if (!view.empty()) {
    return absl::DataLossError("dagnode trailing bytes.");
  }
  if (!(node.hash == hash)) {
    return absl::DataLossError(absl::StrCat(
        "dagnode self-hash mismatch: file recorded ", node.hash.ToHex(),
        " but path is ", hash.ToHex()));
  }
  return node;
}

absl::StatusOr<bool> LocalMerkleDagStore::Exists(absl::string_view tenant_id,
                                                 absl::string_view session_id,
                                                 const Hash256& hash) const {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  return std::filesystem::exists(PathFor(tenant_id, session_id, hash));
}

}  // namespace litert::lm
