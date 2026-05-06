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

#include "runtime/dpm/projection_prefix_cache.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/hash/hasher.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {
namespace {

bool IsZeroHash(const Hash256& hash) {
  for (uint8_t byte : hash.bytes) {
    if (byte != 0) return false;
  }
  return true;
}

ProjectionPrefixCacheHit Miss(absl::string_view reason) {
  ProjectionPrefixCacheHit hit;
  hit.hit = false;
  hit.reason = std::string(reason);
  return hit;
}

absl::Status ValidateIdentity(const ProjectionPrefixCacheIdentity& identity) {
  if (identity.tenant_id.empty()) {
    return absl::InvalidArgumentError(
        "projection prefix cache requires tenant_id.");
  }
  if (identity.session_id.empty()) {
    return absl::InvalidArgumentError(
        "projection prefix cache requires session_id.");
  }
  if (identity.schema_id.empty()) {
    return absl::InvalidArgumentError(
        "projection prefix cache requires schema_id.");
  }
  if (identity.memory_budget_chars == 0) {
    return absl::InvalidArgumentError(
        "projection prefix cache requires memory_budget_chars.");
  }
  if (identity.model_id.empty()) {
    return absl::InvalidArgumentError(
        "projection prefix cache requires model_id.");
  }
  if (IsZeroHash(identity.model_artifact_hash)) {
    return absl::InvalidArgumentError(
        "projection prefix cache requires model_artifact_hash.");
  }
  if (identity.backend_id.empty()) {
    return absl::InvalidArgumentError(
        "projection prefix cache requires backend_id.");
  }
  return absl::OkStatus();
}

absl::Status ValidateEntry(const ProjectionPrefixCacheEntry& entry) {
  RETURN_IF_ERROR(ValidateIdentity(entry.identity));
  if (IsZeroHash(entry.schema_hash)) {
    return absl::InvalidArgumentError(
        "projection prefix cache entry requires schema_hash.");
  }
  if (IsZeroHash(entry.prompt_prefix_hash)) {
    return absl::InvalidArgumentError(
        "projection prefix cache entry requires prompt_prefix_hash.");
  }
  if (entry.prompt_prefix_bytes == 0) {
    return absl::InvalidArgumentError(
        "projection prefix cache entry requires prompt_prefix_bytes.");
  }
  if (entry.event_count == 0) {
    return absl::InvalidArgumentError(
        "projection prefix cache entry requires event_count.");
  }
  if (IsZeroHash(entry.checkpoint_manifest_hash)) {
    return absl::InvalidArgumentError(
        "projection prefix cache entry requires checkpoint_manifest_hash.");
  }
  return absl::OkStatus();
}

bool SameIdentity(const ProjectionPrefixCacheIdentity& a,
                  const ProjectionPrefixCacheIdentity& b,
                  std::string* reason) {
  if (a.tenant_id != b.tenant_id || a.session_id != b.session_id ||
      a.branch_id != b.branch_id) {
    *reason = "tenant, session, or branch mismatch";
    return false;
  }
  if (a.schema_id != b.schema_id) {
    *reason = "schema_id mismatch";
    return false;
  }
  if (a.memory_budget_chars != b.memory_budget_chars) {
    *reason = "memory_budget_chars mismatch";
    return false;
  }
  if (a.model_id != b.model_id ||
      a.model_artifact_hash != b.model_artifact_hash) {
    *reason = "model binding mismatch";
    return false;
  }
  if (a.backend_id != b.backend_id) {
    *reason = "backend_id mismatch";
    return false;
  }
  if (a.hash_algorithm != b.hash_algorithm) {
    *reason = "hash algorithm mismatch";
    return false;
  }
  return true;
}

}  // namespace

Hash256 HashProjectionSchema(HashAlgorithm algo,
                             absl::string_view schema_json) {
  return HashBytes(algo, schema_json);
}

absl::StatusOr<ProjectionPrefixCacheEntry> CreateProjectionPrefixCacheEntry(
    const ProjectionPrefixCacheIdentity& identity,
    absl::string_view schema_json, absl::string_view prompt_prefix,
    uint64_t event_count, const Hash256& checkpoint_manifest_hash) {
  if (auto status = ValidateIdentity(identity); !status.ok()) return status;
  if (schema_json.empty()) {
    return absl::InvalidArgumentError(
        "projection prefix cache requires schema_json.");
  }
  if (prompt_prefix.empty()) {
    return absl::InvalidArgumentError(
        "projection prefix cache requires prompt_prefix bytes.");
  }
  if (event_count == 0) {
    return absl::InvalidArgumentError(
        "projection prefix cache requires at least one covered event.");
  }
  if (IsZeroHash(checkpoint_manifest_hash)) {
    return absl::InvalidArgumentError(
        "projection prefix cache requires checkpoint_manifest_hash.");
  }

  ProjectionPrefixCacheEntry entry;
  entry.identity = identity;
  entry.schema_hash = HashProjectionSchema(identity.hash_algorithm,
                                           schema_json);
  entry.prompt_prefix_hash = HashBytes(identity.hash_algorithm, prompt_prefix);
  entry.prompt_prefix_bytes = prompt_prefix.size();
  entry.event_count = event_count;
  entry.checkpoint_manifest_hash = checkpoint_manifest_hash;
  return entry;
}

ProjectionPrefixCacheHit EvaluateProjectionPrefixCacheHit(
    const ProjectionPrefixCacheEntry& entry,
    const ProjectionPrefixCacheIdentity& request_identity,
    absl::string_view schema_json, absl::string_view full_prompt) {
  std::string reason;
  if (!SameIdentity(entry.identity, request_identity, &reason)) {
    return Miss(reason);
  }
  if (schema_json.empty()) {
    return Miss("schema_json is empty");
  }
  const Hash256 request_schema_hash =
      HashProjectionSchema(request_identity.hash_algorithm, schema_json);
  if (entry.schema_hash != request_schema_hash) {
    return Miss("schema_json hash mismatch");
  }
  if (entry.prompt_prefix_bytes == 0 ||
      entry.prompt_prefix_bytes > full_prompt.size()) {
    return Miss(absl::StrCat("cached prefix length ",
                             entry.prompt_prefix_bytes,
                             " is not a prefix of prompt length ",
                             full_prompt.size()));
  }

  const size_t prompt_prefix_bytes =
      static_cast<size_t>(entry.prompt_prefix_bytes);
  const absl::string_view observed_prefix(full_prompt.data(),
                                          prompt_prefix_bytes);
  ProjectionPrefixCacheHit hit;
  hit.observed_prefix_hash =
      HashBytes(request_identity.hash_algorithm, observed_prefix);
  if (hit.observed_prefix_hash != entry.prompt_prefix_hash) {
    hit.hit = false;
    hit.reason = "prompt prefix bytes changed";
    return hit;
  }

  hit.hit = true;
  hit.reason = "exact projection prefix cache hit";
  const absl::string_view suffix = full_prompt.substr(prompt_prefix_bytes);
  hit.suffix.assign(suffix.data(), suffix.size());
  return hit;
}

absl::Status InMemoryProjectionPrefixCache::Store(
    ProjectionPrefixCacheEntry entry) {
  RETURN_IF_ERROR(ValidateEntry(entry));
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.push_back(std::move(entry));
  return absl::OkStatus();
}

absl::StatusOr<ProjectionPrefixCacheLookup>
InMemoryProjectionPrefixCache::FindLongestPrefixHit(
    const ProjectionPrefixCacheIdentity& request_identity,
    absl::string_view schema_json, absl::string_view full_prompt) const {
  RETURN_IF_ERROR(ValidateIdentity(request_identity));
  std::lock_guard<std::mutex> lock(mutex_);
  ProjectionPrefixCacheLookup best;
  bool found = false;
  for (const ProjectionPrefixCacheEntry& entry : entries_) {
    ProjectionPrefixCacheHit hit = EvaluateProjectionPrefixCacheHit(
        entry, request_identity, schema_json, full_prompt);
    if (!hit.hit) continue;
    if (!found ||
        entry.prompt_prefix_bytes > best.entry.prompt_prefix_bytes) {
      best.entry = entry;
      best.hit = std::move(hit);
      found = true;
    }
  }
  if (!found) {
    return absl::NotFoundError("no exact projection prefix cache hit.");
  }
  return best;
}

}  // namespace litert::lm
