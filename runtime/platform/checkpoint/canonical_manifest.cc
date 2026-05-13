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

#include "runtime/platform/checkpoint/canonical_manifest.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "runtime/platform/hash/hasher.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {
namespace {

constexpr std::array<char, 12> kManifestMagic = {
    'D', 'P', 'M', 'M', 'A', 'N', 'I', 'F', 'E', 'S', 'T', '\n'};

void AppendU32(uint32_t v, std::string* out) {
  for (int i = 0; i < 4; ++i)
    out->push_back(static_cast<char>((v >> (i * 8)) & 0xff));
}

void AppendU64(uint64_t v, std::string* out) {
  for (int i = 0; i < 8; ++i)
    out->push_back(static_cast<char>((v >> (i * 8)) & 0xff));
}

void AppendI64(int64_t v, std::string* out) {
  AppendU64(static_cast<uint64_t>(v), out);
}

// Length-prefixed string: u32 size followed by bytes. Empty strings are
// encoded as a u32 zero so that a present-but-empty field is distinct
// from a different field at the same position.
void AppendLpStr(absl::string_view s, std::string* out) {
  AppendU32(static_cast<uint32_t>(s.size()), out);
  out->append(s.data(), s.size());
}

void AppendHash(const Hash256& h, std::string* out) {
  out->append(reinterpret_cast<const char*>(h.bytes.data()), h.bytes.size());
}

absl::StatusOr<uint32_t> ReadU32(absl::string_view* view) {
  if (view->size() < 4) return absl::DataLossError("manifest truncated u32.");
  const unsigned char* p =
      reinterpret_cast<const unsigned char*>(view->data());
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(p[i]) << (i * 8);
  }
  view->remove_prefix(4);
  return v;
}

absl::StatusOr<uint64_t> ReadU64(absl::string_view* view) {
  if (view->size() < 8) return absl::DataLossError("manifest truncated u64.");
  const unsigned char* p =
      reinterpret_cast<const unsigned char*>(view->data());
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(p[i]) << (i * 8);
  }
  view->remove_prefix(8);
  return v;
}

absl::StatusOr<int64_t> ReadI64(absl::string_view* view) {
  ASSIGN_OR_RETURN(uint64_t v, ReadU64(view));
  return static_cast<int64_t>(v);
}

absl::StatusOr<std::string> ReadLpStr(absl::string_view* view) {
  ASSIGN_OR_RETURN(uint32_t size, ReadU32(view));
  if (view->size() < size) return absl::DataLossError("manifest truncated str.");
  std::string out(view->data(), size);
  view->remove_prefix(size);
  return out;
}

absl::StatusOr<Hash256> ReadHash(absl::string_view* view) {
  if (view->size() < 32) return absl::DataLossError("manifest truncated hash.");
  Hash256 hash;
  std::memcpy(hash.bytes.data(), view->data(), hash.bytes.size());
  view->remove_prefix(hash.bytes.size());
  return hash;
}

}  // namespace

absl::StatusOr<std::string> EncodeCanonicalManifest(
    const CanonicalManifestInput& input) {
  std::string out;
  out.reserve(256 + input.parent_hashes.size() * 32);
  out.append(kManifestMagic.data(), kManifestMagic.size());
  AppendU32(kCanonicalManifestVersion, &out);

  // Identity (3 length-prefixed strings).
  AppendLpStr(input.tenant_id, &out);
  AppendLpStr(input.session_id, &out);
  AppendLpStr(input.branch_id, &out);

  // Level.
  AppendU32(input.level, &out);
  AppendU32(input.compaction_interval, &out);

  // Parent DAG edges. The order is preserved (caller is responsible for
  // sorting if a deterministic merge-DAG identity is desired across
  // different walk orders; the canonical encoding does not re-sort because
  // a producer that intentionally records a topological ordering must be
  // able to reflect that in the digest).
  AppendU32(static_cast<uint32_t>(input.parent_hashes.size()), &out);
  for (const Hash256& h : input.parent_hashes) AppendHash(h, &out);

  // Producer.
  AppendLpStr(input.architecture_tag, &out);
  AppendLpStr(input.producer_id, &out);
  AppendLpStr(input.runtime_version, &out);

  // Model binding.
  AppendHash(input.model_artifact_hash, &out);
  AppendLpStr(input.model_id, &out);
  AppendLpStr(input.schema_id, &out);
  AppendHash(input.schema_hash, &out);
  AppendU32(input.model_class, &out);
  AppendU32(input.num_layers, &out);
  AppendU32(input.num_kv_heads, &out);
  AppendU32(input.head_dim, &out);

  // KV.
  AppendU32(input.kv_dtype, &out);

  // Coverage / body.
  AppendU64(input.event_range_start, &out);
  AppendU64(input.event_range_end, &out);
  AppendU64(input.base_event_index, &out);
  AppendHash(input.body_hash, &out);
  AppendU32(input.body_size_bytes, &out);
  AppendI64(input.created_unix_micros, &out);

  return out;
}

absl::StatusOr<CanonicalManifestInput> DecodeCanonicalManifest(
    absl::string_view bytes) {
  if (bytes.size() < kManifestMagic.size() ||
      bytes.substr(0, kManifestMagic.size()) !=
          absl::string_view(kManifestMagic.data(), kManifestMagic.size())) {
    return absl::DataLossError("manifest missing magic header.");
  }
  bytes.remove_prefix(kManifestMagic.size());
  ASSIGN_OR_RETURN(uint32_t version, ReadU32(&bytes));
  if (version != kCanonicalManifestVersion) {
    return absl::DataLossError("unsupported canonical manifest version.");
  }

  CanonicalManifestInput input;
  ASSIGN_OR_RETURN(input.tenant_id, ReadLpStr(&bytes));
  ASSIGN_OR_RETURN(input.session_id, ReadLpStr(&bytes));
  ASSIGN_OR_RETURN(input.branch_id, ReadLpStr(&bytes));

  ASSIGN_OR_RETURN(input.level, ReadU32(&bytes));
  ASSIGN_OR_RETURN(input.compaction_interval, ReadU32(&bytes));

  ASSIGN_OR_RETURN(uint32_t parent_count, ReadU32(&bytes));
  if (parent_count >
      std::numeric_limits<uint32_t>::max() / sizeof(Hash256)) {
    return absl::ResourceExhaustedError("manifest parent count is too large.");
  }
  input.parent_hashes.reserve(parent_count);
  for (uint32_t i = 0; i < parent_count; ++i) {
    ASSIGN_OR_RETURN(Hash256 parent, ReadHash(&bytes));
    input.parent_hashes.push_back(parent);
  }

  ASSIGN_OR_RETURN(input.architecture_tag, ReadLpStr(&bytes));
  ASSIGN_OR_RETURN(input.producer_id, ReadLpStr(&bytes));
  ASSIGN_OR_RETURN(input.runtime_version, ReadLpStr(&bytes));

  ASSIGN_OR_RETURN(input.model_artifact_hash, ReadHash(&bytes));
  ASSIGN_OR_RETURN(input.model_id, ReadLpStr(&bytes));
  ASSIGN_OR_RETURN(input.schema_id, ReadLpStr(&bytes));
  ASSIGN_OR_RETURN(input.schema_hash, ReadHash(&bytes));
  ASSIGN_OR_RETURN(input.model_class, ReadU32(&bytes));
  ASSIGN_OR_RETURN(input.num_layers, ReadU32(&bytes));
  ASSIGN_OR_RETURN(input.num_kv_heads, ReadU32(&bytes));
  ASSIGN_OR_RETURN(input.head_dim, ReadU32(&bytes));

  ASSIGN_OR_RETURN(input.kv_dtype, ReadU32(&bytes));

  ASSIGN_OR_RETURN(input.event_range_start, ReadU64(&bytes));
  ASSIGN_OR_RETURN(input.event_range_end, ReadU64(&bytes));
  ASSIGN_OR_RETURN(input.base_event_index, ReadU64(&bytes));
  ASSIGN_OR_RETURN(input.body_hash, ReadHash(&bytes));
  ASSIGN_OR_RETURN(input.body_size_bytes, ReadU32(&bytes));
  ASSIGN_OR_RETURN(input.created_unix_micros, ReadI64(&bytes));

  if (!bytes.empty()) {
    return absl::DataLossError("manifest trailing bytes.");
  }
  return input;
}

absl::StatusOr<Hash256> ComputeManifestHash(
    HashAlgorithm algo, const CanonicalManifestInput& input) {
  auto bytes = EncodeCanonicalManifest(input);
  if (!bytes.ok()) return bytes.status();
  return HashBytes(algo, *bytes);
}

}  // namespace litert::lm
