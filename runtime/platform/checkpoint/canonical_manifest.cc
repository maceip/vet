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
#include <string>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "runtime/platform/hash/hasher.h"

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
  AppendU32(input.model_class, &out);
  AppendU32(input.num_layers, &out);
  AppendU32(input.num_kv_heads, &out);
  AppendU32(input.head_dim, &out);

  // KV.
  AppendU32(input.kv_dtype, &out);

  // Coverage / body.
  AppendU64(input.base_event_index, &out);
  AppendHash(input.body_hash, &out);
  AppendU32(input.body_size_bytes, &out);
  AppendI64(input.created_unix_micros, &out);

  return out;
}

absl::StatusOr<Hash256> ComputeManifestHash(
    HashAlgorithm algo, const CanonicalManifestInput& input) {
  auto bytes = EncodeCanonicalManifest(input);
  if (!bytes.ok()) return bytes.status();
  return HashBytes(algo, *bytes);
}

}  // namespace litert::lm
