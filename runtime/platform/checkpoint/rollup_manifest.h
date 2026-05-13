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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_ROLLUP_MANIFEST_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_ROLLUP_MANIFEST_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "runtime/platform/checkpoint/canonical_manifest.h"
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {

struct RollupChildRef {
  Hash256 manifest_hash;
  Hash256 body_hash;
  uint64_t event_range_start = 0;
  uint64_t event_range_end = 0;
  std::string schema_id;
  std::string projection_model_id;
};

RollupChildRef RollupChildRefFromManifest(
    const Hash256& manifest_hash, const CanonicalManifestInput& manifest);

absl::Status ValidateRollupChildrenForWrite(
    uint64_t parent_event_range_start, uint64_t parent_event_range_end,
    const std::string& parent_schema_id,
    const std::string& parent_projection_model_id,
    const std::vector<RollupChildRef>& children);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_ROLLUP_MANIFEST_H_
