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

#include <vector>

#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "runtime/platform/checkpoint/canonical_manifest.h"
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {
namespace {

using ::testing::HasSubstr;

Hash256 H(absl::string_view bytes) {
  return HashBytes(HashAlgorithm::kBlake3, bytes);
}

RollupChildRef Child(int i, uint64_t start, uint64_t end) {
  return RollupChildRef{
      .manifest_hash = H(absl::StrCat("manifest-", i)),
      .body_hash = H(absl::StrCat("body-", i)),
      .event_range_start = start,
      .event_range_end = end,
      .schema_id = "incident_response_v1",
      .projection_model_id = "pinned-projection-model",
  };
}

TEST(RollupManifestTest, AcceptsContiguousImmediateChildren) {
  EXPECT_TRUE(ValidateRollupChildrenForWrite(
                  10, 40, "incident_response_v1",
                  "pinned-projection-model",
                  {Child(2, 20, 30), Child(1, 10, 20), Child(3, 30, 40)})
                  .ok());
}

TEST(RollupManifestTest, RejectsGapAndOverlap) {
  absl::Status gap = ValidateRollupChildrenForWrite(
      0, 30, "incident_response_v1", "pinned-projection-model",
      {Child(1, 0, 10), Child(2, 20, 30)});
  EXPECT_FALSE(gap.ok());
  EXPECT_THAT(gap.message(), HasSubstr("no gaps or overlaps"));

  absl::Status overlap = ValidateRollupChildrenForWrite(
      0, 30, "incident_response_v1", "pinned-projection-model",
      {Child(1, 0, 20), Child(2, 10, 30)});
  EXPECT_FALSE(overlap.ok());
  EXPECT_THAT(overlap.message(), HasSubstr("no gaps or overlaps"));
}

TEST(RollupManifestTest, RejectsMixedSchemaOrModel) {
  RollupChildRef mixed_schema = Child(2, 10, 20);
  mixed_schema.schema_id = "claim_review_v2";
  EXPECT_FALSE(ValidateRollupChildrenForWrite(
                   0, 20, "incident_response_v1",
                   "pinned-projection-model",
                   {Child(1, 0, 10), mixed_schema})
                   .ok());

  RollupChildRef mixed_model = Child(2, 10, 20);
  mixed_model.projection_model_id = "other-model";
  EXPECT_FALSE(ValidateRollupChildrenForWrite(
                   0, 20, "incident_response_v1",
                   "pinned-projection-model",
                   {Child(1, 0, 10), mixed_model})
                   .ok());
}

TEST(RollupManifestTest, ChildRefComesFromManifestRangeAndModelFields) {
  CanonicalManifestInput manifest;
  manifest.body_hash = H("body");
  manifest.event_range_start = 40;
  manifest.event_range_end = 55;
  manifest.schema_id = "incident_response_v1";
  manifest.model_id = "pinned-projection-model";
  const Hash256 manifest_hash = H("manifest");

  const RollupChildRef child =
      RollupChildRefFromManifest(manifest_hash, manifest);
  EXPECT_EQ(child.manifest_hash, manifest_hash);
  EXPECT_EQ(child.body_hash, manifest.body_hash);
  EXPECT_EQ(child.event_range_start, 40);
  EXPECT_EQ(child.event_range_end, 55);
  EXPECT_EQ(child.schema_id, "incident_response_v1");
  EXPECT_EQ(child.projection_model_id, "pinned-projection-model");
}

}  // namespace
}  // namespace litert::lm
