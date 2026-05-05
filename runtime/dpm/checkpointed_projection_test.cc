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

#include "runtime/dpm/checkpointed_projection.h"

#include <filesystem>
#include <string>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "runtime/dpm/dpm_projector.h"
#include "runtime/dpm/event.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/platform/checkpoint/checkpoint_store.h"
#include "runtime/platform/checkpoint/local_filesystem_checkpoint_store.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/platform/provenance/local_merkle_dag_store.h"
#include "runtime/platform/provenance/provenance_query.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

std::filesystem::path TestRoot(absl::string_view name) {
  std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / std::string(name);
  std::filesystem::remove_all(path);
  return path;
}

class RecordingRunner : public DPMInferenceRunner {
 public:
  explicit RecordingRunner(std::string response)
      : response_(std::move(response)) {}

  absl::StatusOr<std::string> Generate(
      absl::string_view prompt, const DPMInferenceConfig& config) override {
    prompts.push_back(std::string(prompt));
    configs.push_back(config);
    return response_;
  }

  std::vector<std::string> prompts;
  std::vector<DPMInferenceConfig> configs;

 private:
  std::string response_;
};

ProjectionCheckpointConfig BaseConfig() {
  ProjectionCheckpointConfig config;
  config.projection.schema_id = "incident_response_v1";
  config.projection.schema_json =
      R"json({"Facts":["string with [i]"],"Reasoning":["string with [i]"],"Compliance":["string with [i]"]})json";
  config.projection.model_id = "pinned-test-model";
  config.projection.memory_budget_chars = 1338;
  config.model_artifact_hash = HashBytes(HashAlgorithm::kBlake3, "model");
  config.architecture_tag = "x86_64-cpu";
  config.producer_id = "checkpointed_projection_test";
  config.runtime_version = "test";
  config.created_unix_micros = 1777390000000000;
  return config;
}

TEST(CheckpointedProjectionTest, StoresProjectedMemoryNotRawLog) {
  EventSourcedLog log(TestRoot("checkpoint_projection_log"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .payload = "raw credential should not be checkpoint payload",
      .timestamp_us = 1,
  }));
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "tool observed T1021 lateral movement",
      .timestamp_us = 2,
  }));

  RecordingRunner runner(
      R"json({"Facts":["T1021 lateral movement [2]"],"Reasoning":["tool evidence supports escalation [2]"],"Compliance":["preserve incident trail [1]"]})json");
  DPMProjector projector(&runner);
  const std::filesystem::path checkpoint_root =
      TestRoot("checkpoint_projection_store");
  LocalFilesystemCheckpointStore store(checkpoint_root);
  LocalMerkleDagStore dag(checkpoint_root);

  ASSERT_OK_AND_ASSIGN(
      ProjectionCheckpoint checkpoint,
      CreateProjectionCheckpoint(log, &projector, BaseConfig(), &store, &dag));

  EXPECT_THAT(checkpoint.projected_memory, HasSubstr("T1021 lateral movement"));
  EXPECT_THAT(checkpoint.projected_memory,
              Not(HasSubstr("raw credential should not be checkpoint payload")));
  EXPECT_EQ(checkpoint.event_count, 2);
  ASSERT_EQ(runner.configs.size(), 1);
  EXPECT_TRUE(runner.configs[0].fresh_context);

  ASSERT_OK_AND_ASSIGN(
      std::string loaded,
      LoadProjectionCheckpoint(log.identity(), checkpoint.manifest_hash,
                               &store));
  EXPECT_EQ(loaded, checkpoint.projected_memory);

  ASSERT_OK_AND_ASSIGN(CheckpointStore::ManifestRecord manifest,
                       store.GetManifest("tenant-a", "session-1",
                                         checkpoint.manifest_hash));
  EXPECT_EQ(manifest.referenced_body_hash, checkpoint.body_hash);
  ASSERT_OK_AND_ASSIGN(
      ProvenanceChain chain,
      GetCheckpointProvenance(dag, "tenant-a", "session-1",
                              checkpoint.manifest_hash));
  ASSERT_EQ(chain.nodes.size(), 1);
  EXPECT_EQ(chain.nodes[0].hash, checkpoint.manifest_hash);
}

TEST(CheckpointedProjectionTest, ParentHashLinksNextCheckpoint) {
  EventSourcedLog log(TestRoot("checkpoint_projection_parent_log"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-2",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .payload = "first event",
      .timestamp_us = 1,
  }));

  RecordingRunner first_runner(
      R"json({"Facts":["first event [1]"],"Reasoning":["initial state [1]"],"Compliance":["ok [1]"]})json");
  DPMProjector first_projector(&first_runner);
  const std::filesystem::path checkpoint_root =
      TestRoot("checkpoint_projection_parent_store");
  LocalFilesystemCheckpointStore store(checkpoint_root);
  LocalMerkleDagStore dag(checkpoint_root);
  ProjectionCheckpointConfig first_config = BaseConfig();
  ASSERT_OK_AND_ASSIGN(
      ProjectionCheckpoint first,
      CreateProjectionCheckpoint(log, &first_projector, first_config, &store,
                                 &dag));

  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .payload = "second event",
      .timestamp_us = 2,
  }));
  RecordingRunner second_runner(
      R"json({"Facts":["second event [2]"],"Reasoning":["extends first [1]"],"Compliance":["ok [1]"]})json");
  DPMProjector second_projector(&second_runner);
  ProjectionCheckpointConfig second_config = BaseConfig();
  second_config.parent_manifest_hashes = {first.manifest_hash};
  second_config.created_unix_micros += 1;
  ASSERT_OK_AND_ASSIGN(
      ProjectionCheckpoint second,
      CreateProjectionCheckpoint(log, &second_projector, second_config, &store,
                                 &dag));

  ASSERT_OK_AND_ASSIGN(
      ProvenanceChain chain,
      GetCheckpointProvenance(dag, "tenant-a", "session-2",
                              second.manifest_hash));
  ASSERT_EQ(chain.nodes.size(), 2);
  EXPECT_EQ(chain.nodes[0].hash, second.manifest_hash);
  EXPECT_EQ(chain.nodes[1].hash, first.manifest_hash);
}

}  // namespace
}  // namespace litert::lm
