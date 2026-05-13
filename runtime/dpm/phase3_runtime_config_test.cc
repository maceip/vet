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

#include "runtime/dpm/phase3_runtime_config.h"

#include <limits>

#include "absl/status/status.h"  // from @com_google_absl
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "runtime/platform/audit/audit_certificate.h"

namespace litert::lm {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

TEST(Phase3RuntimeConfigTest, DefaultsAreStrictAndValid) {
  Phase3RuntimeConfig config;

  EXPECT_TRUE(ValidatePhase3RuntimeConfig(config).ok());

  AuditedProjectionCheckpointRequest request;
  DPMProjector::ProjectionConfig projection;
  projection.correction_repair_attempts = 99;
  ASSERT_TRUE(ApplyPhase3RuntimeConfig(config, &request, &projection).ok());
  EXPECT_EQ(request.max_allowed_drift_score, 0.0);
  EXPECT_FALSE(request.require_valid_signature);
  EXPECT_EQ(request.min_valid_signatures, 1);
  EXPECT_TRUE(request.allowed_signature_algorithms.empty());
  EXPECT_EQ(projection.correction_repair_attempts, 1);
}

TEST(Phase3RuntimeConfigTest, AppliesSignedAuditPolicy) {
  Phase3RuntimeConfig config;
  config.audit_gate.require_valid_signature = true;
  config.audit_gate.min_valid_signatures = 2;
  config.audit_gate.allowed_signature_algorithms = {
      kAuditSignatureAlgorithmMlDsa65,
      kAuditSignatureAlgorithmMlDsa87,
  };
  config.correction_replay.correction_repair_attempts = 2;

  AuditedProjectionCheckpointRequest request;
  DPMProjector::ProjectionConfig projection;
  ASSERT_TRUE(ApplyPhase3RuntimeConfig(config, &request, &projection).ok());

  EXPECT_TRUE(request.require_valid_signature);
  EXPECT_EQ(request.min_valid_signatures, 2);
  EXPECT_THAT(request.allowed_signature_algorithms,
              ElementsAre(kAuditSignatureAlgorithmMlDsa65,
                          kAuditSignatureAlgorithmMlDsa87));
  EXPECT_EQ(projection.correction_repair_attempts, 2);
}

TEST(Phase3RuntimeConfigTest, RejectsUnsafeOrAmbiguousPolicies) {
  Phase3RuntimeConfig config;
  config.audit_gate.max_allowed_drift_score =
      std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(ValidatePhase3RuntimeConfig(config).code(),
            absl::StatusCode::kInvalidArgument);

  config = Phase3RuntimeConfig();
  config.audit_gate.require_valid_signature = true;
  absl::Status status = ValidatePhase3RuntimeConfig(config);
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(status.message(),
              HasSubstr("explicit allowed signature algorithms"));

  config = Phase3RuntimeConfig();
  config.correction_replay.require_machine_actionable_blocking_corrections =
      false;
  status = ValidatePhase3RuntimeConfig(config);
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(status.message(), HasSubstr("machine-actionable"));
}

TEST(Phase3RuntimeConfigTest, ApplyRejectsNullTargets) {
  Phase3RuntimeConfig config;
  AuditedProjectionCheckpointRequest request;
  DPMProjector::ProjectionConfig projection;

  EXPECT_EQ(ApplyPhase3RuntimeConfig(config, nullptr, &projection).code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(ApplyPhase3RuntimeConfig(config, &request, nullptr).code(),
            absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace litert::lm
