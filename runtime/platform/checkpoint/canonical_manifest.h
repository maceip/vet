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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_CANONICAL_MANIFEST_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_CANONICAL_MANIFEST_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {

// Canonical, language-independent encoding of a CheckpointAbi for the
// purposes of computing manifest_hash. The substrate intentionally does
// not link the proto generated code; callers (the DPM layer) extract the
// fields and pass them as primitive types, and this module produces the
// byte-deterministic encoding both producer and verifier hash.
//
// The encoding is a length-prefixed framed binary so that adding new
// fields in a future ABI version cannot collide with old field bytes
// (every field is preceded by its byte length). The on-the-wire format is
// versioned via kCanonicalManifestVersion; bumping the version is a
// breaking digest change that all replays must opt into explicitly.
struct CanonicalManifestInput {
  // Identity
  std::string tenant_id;
  std::string session_id;
  std::string branch_id;

  // Level
  uint32_t level = 0;
  uint32_t compaction_interval = 0;

  // Parent DAG edges
  std::vector<Hash256> parent_hashes;

  // Producer
  std::string architecture_tag;
  std::string producer_id;
  std::string runtime_version;

  // Model binding
  Hash256 model_artifact_hash;
  std::string model_id;
  uint32_t model_class = 0;
  uint32_t num_layers = 0;
  uint32_t num_kv_heads = 0;
  uint32_t head_dim = 0;

  // KV transport encoding
  uint32_t kv_dtype = 0;

  // Coverage and body
  uint64_t base_event_index = 0;
  Hash256 body_hash;
  uint32_t body_size_bytes = 0;
  int64_t created_unix_micros = 0;
};

// Wire-format version. Must be bumped in lockstep with any change that
// affects the byte layout below.
inline constexpr uint32_t kCanonicalManifestVersion = 1;

// Returns the canonical bytes that would be hashed to produce
// manifest_hash. Emitted independently of any hash algorithm so tests can
// pin against expected bytes without hashing.
absl::StatusOr<std::string> EncodeCanonicalManifest(
    const CanonicalManifestInput& input);

// Computes manifest_hash by hashing the canonical encoding with the given
// algorithm.
absl::StatusOr<Hash256> ComputeManifestHash(
    HashAlgorithm algo, const CanonicalManifestInput& input);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_CANONICAL_MANIFEST_H_
