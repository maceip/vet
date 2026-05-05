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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_DPM_PROJECTOR_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_DPM_PROJECTOR_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/proto/sampler_params.pb.h"

namespace litert::lm {

struct DPMInferenceConfig {
  int max_output_tokens = 4096;
  int seed = 42;
  float temperature = 0.0f;
  bool fresh_context = true;
  std::string model_id;
};

proto::SamplerParameters CreateDPMSamplerParameters(
    const DPMInferenceConfig& config);

class DPMInferenceRunner {
 public:
  virtual ~DPMInferenceRunner() = default;
  virtual absl::StatusOr<std::string> Generate(
      absl::string_view prompt, const DPMInferenceConfig& config) = 0;
};

class DPMProjector {
 public:
  struct ProjectionConfig {
    size_t max_tokens = 4096;
    size_t memory_budget_chars = 4096;
    size_t max_event_log_chars = 1 << 20;
    std::string schema_id;
    std::string schema_json;
    int seed = 42;
    float temperature = 0.0f;
    std::string model_id;
  };

  explicit DPMProjector(DPMInferenceRunner* runner);

  absl::StatusOr<std::string> Project(const EventSourcedLog& log,
                                      const ProjectionConfig& config);
  absl::StatusOr<std::string> ProjectRange(const EventSourcedLog& log,
                                           uint64_t event_range_start,
                                           uint64_t event_range_end,
                                           const ProjectionConfig& config);
  absl::StatusOr<std::string> CreateProjectionPrompt(
      const EventSourcedLog& log, const ProjectionConfig& config) const;
  absl::StatusOr<std::string> CreateProjectionPromptForRange(
      const EventSourcedLog& log, uint64_t event_range_start,
      uint64_t event_range_end, const ProjectionConfig& config) const;

 private:
  DPMInferenceRunner* runner_;
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_DPM_PROJECTOR_H_
