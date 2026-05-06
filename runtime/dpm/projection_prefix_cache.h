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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_PROJECTION_PREFIX_CACHE_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_PROJECTION_PREFIX_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {

// Identity fields that make a projection-prefix KV checkpoint reusable.
// Reuse is exact: the prompt bytes already represented by the cached KV
// checkpoint must still be a byte-identical prefix of the current prompt.
struct ProjectionPrefixCacheIdentity {
  std::string tenant_id;
  std::string session_id;
  std::string branch_id;

  std::string schema_id;
  size_t memory_budget_chars = 0;

  std::string model_id;
  Hash256 model_artifact_hash;
  std::string backend_id;

  HashAlgorithm hash_algorithm = HashAlgorithm::kBlake3;
};

// Stored metadata for a backend-owned KV checkpoint. checkpoint_manifest_hash
// points at the checkpoint ABI/provenance node; prompt_prefix_hash proves the
// text prefix the checkpoint represents.
struct ProjectionPrefixCacheEntry {
  ProjectionPrefixCacheIdentity identity;
  Hash256 schema_hash;
  Hash256 prompt_prefix_hash;
  uint64_t prompt_prefix_bytes = 0;
  uint64_t event_count = 0;
  Hash256 checkpoint_manifest_hash;
};

struct ProjectionPrefixCacheHit {
  bool hit = false;
  std::string suffix;
  std::string reason;
  Hash256 observed_prefix_hash;
};

struct ProjectionPrefixCacheLookup {
  ProjectionPrefixCacheEntry entry;
  ProjectionPrefixCacheHit hit;
};

// Hashes schema_json with the same algorithm used by cache entries. Keeping
// the raw schema out of the key lets metadata stay compact while still
// invalidating on every schema byte change.
Hash256 HashProjectionSchema(HashAlgorithm algo, absl::string_view schema_json);

// Creates an exact-reuse entry for a prompt prefix that has already been
// prefetched into a backend KV checkpoint. prompt_prefix is usually the full
// projection prompt for the previous append-only log generation.
absl::StatusOr<ProjectionPrefixCacheEntry> CreateProjectionPrefixCacheEntry(
    const ProjectionPrefixCacheIdentity& identity,
    absl::string_view schema_json, absl::string_view prompt_prefix,
    uint64_t event_count, const Hash256& checkpoint_manifest_hash);

// Evaluates whether entry can be reused for full_prompt. On a hit, suffix is
// the bytes after entry.prompt_prefix_bytes; feeding the cached KV checkpoint
// plus suffix is byte-equivalent to prefilling full_prompt from scratch.
ProjectionPrefixCacheHit EvaluateProjectionPrefixCacheHit(
    const ProjectionPrefixCacheEntry& entry,
    const ProjectionPrefixCacheIdentity& request_identity,
    absl::string_view schema_json, absl::string_view full_prompt);

class ProjectionPrefixCache {
 public:
  virtual ~ProjectionPrefixCache() = default;

  virtual absl::Status Store(ProjectionPrefixCacheEntry entry) = 0;

  // Finds the longest cached prefix that is an exact byte-prefix of
  // full_prompt. Returns NotFound when no cached KV checkpoint can be reused.
  virtual absl::StatusOr<ProjectionPrefixCacheLookup> FindLongestPrefixHit(
      const ProjectionPrefixCacheIdentity& request_identity,
      absl::string_view schema_json, absl::string_view full_prompt) const = 0;
};

// Process-local implementation for tests and first benchmarks. Production
// adapters should store the same entries in the hot metadata tier.
class InMemoryProjectionPrefixCache : public ProjectionPrefixCache {
 public:
  absl::Status Store(ProjectionPrefixCacheEntry entry) override;

  absl::StatusOr<ProjectionPrefixCacheLookup> FindLongestPrefixHit(
      const ProjectionPrefixCacheIdentity& request_identity,
      absl::string_view schema_json,
      absl::string_view full_prompt) const override;

 private:
  mutable std::mutex mutex_;
  std::vector<ProjectionPrefixCacheEntry> entries_;
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_PROJECTION_PREFIX_CACHE_H_
