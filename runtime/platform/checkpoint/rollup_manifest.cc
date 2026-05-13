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

#include "runtime/platform/checkpoint/rollup_manifest.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "runtime/platform/checkpoint/canonical_manifest.h"
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {
namespace {

bool IsZeroHash(const Hash256& hash) {
  static const Hash256 kZero;
  return hash == kZero;
}

}  // namespace

RollupChildRef RollupChildRefFromManifest(
    const Hash256& manifest_hash, const CanonicalManifestInput& manifest) {
  return RollupChildRef{
      .manifest_hash = manifest_hash,
      .body_hash = manifest.body_hash,
      .event_range_start = manifest.event_range_start,
      .event_range_end = manifest.event_range_end,
      .schema_id = manifest.schema_id,
      .projection_model_id = manifest.model_id,
  };
}

absl::Status ValidateRollupChildrenForWrite(
    uint64_t parent_event_range_start, uint64_t parent_event_range_end,
    const std::string& parent_schema_id,
    const std::string& parent_projection_model_id,
    const std::vector<RollupChildRef>& children) {
  if (parent_event_range_end <= parent_event_range_start) {
    return absl::InvalidArgumentError(
        "rollup parent range must be non-empty and half-open.");
  }
  if (parent_schema_id.empty() || parent_projection_model_id.empty()) {
    return absl::InvalidArgumentError(
        "rollup parent requires schema_id and projection model id.");
  }
  if (children.empty()) {
    return absl::InvalidArgumentError(
        "rollup requires at least one child range.");
  }

  std::vector<RollupChildRef> sorted = children;
  std::sort(sorted.begin(), sorted.end(), [](const RollupChildRef& a,
                                             const RollupChildRef& b) {
    if (a.event_range_start != b.event_range_start) {
      return a.event_range_start < b.event_range_start;
    }
    return a.event_range_end < b.event_range_end;
  });

  uint64_t next_start = parent_event_range_start;
  for (const RollupChildRef& child : sorted) {
    if (IsZeroHash(child.manifest_hash) || IsZeroHash(child.body_hash)) {
      return absl::InvalidArgumentError(
          "rollup child requires manifest_hash and body_hash.");
    }
    if (child.event_range_end <= child.event_range_start) {
      return absl::InvalidArgumentError(
          "rollup child range must be non-empty and half-open.");
    }
    if (child.schema_id != parent_schema_id ||
        child.projection_model_id != parent_projection_model_id) {
      return absl::InvalidArgumentError(
          "rollup children must share schema_id and projection model id.");
    }
    if (child.event_range_start != next_start) {
      return absl::InvalidArgumentError(absl::StrCat(
          "rollup children must cover the parent range with no gaps or "
          "overlaps; expected next child start ",
          next_start, " but got ", child.event_range_start));
    }
    next_start = child.event_range_end;
  }
  if (next_start != parent_event_range_end) {
    return absl::InvalidArgumentError(absl::StrCat(
        "rollup children do not cover parent range end; expected ",
        parent_event_range_end, " but got ", next_start));
  }
  return absl::OkStatus();
}

}  // namespace litert::lm
