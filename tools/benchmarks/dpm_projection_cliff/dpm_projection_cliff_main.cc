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

#include <fstream>
#include <iostream>
#include <string>

#include "absl/flags/flag.h"  // from @com_google_absl
#include "absl/flags/parse.h"  // from @com_google_absl
#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "tools/benchmarks/dpm_projection_cliff/dpm_projection_cliff.h"

ABSL_FLAG(std::string, config, "",
          "Path to a benchmark config YAML (see configs/).");
ABSL_FLAG(std::string, output_jsonl, "",
          "Where to append JSONL rows. The driver appends; the file may "
          "already exist.");

namespace {

absl::Status RunMain() {
  const std::string config_path = absl::GetFlag(FLAGS_config);
  const std::string output_path = absl::GetFlag(FLAGS_output_jsonl);
  if (config_path.empty()) {
    return absl::InvalidArgumentError("--config is required.");
  }
  if (output_path.empty()) {
    return absl::InvalidArgumentError("--output_jsonl is required.");
  }
  auto cfg = litert::lm::bench::LoadConfig(config_path);
  if (!cfg.ok()) return cfg.status();

  std::ofstream out(output_path, std::ios::out | std::ios::app);
  if (!out.is_open()) {
    return absl::InternalError(
        absl::StrCat("cannot open output ", output_path));
  }

  uint64_t emitted = 0;
  for (const std::string& condition : cfg->conditions) {
    for (uint64_t traj : cfg->trajectory_chars) {
      for (uint64_t budget : cfg->memory_budget_chars) {
        for (uint32_t r = 0; r < cfg->repeats; ++r) {
          auto row = litert::lm::bench::RunOneCell(*cfg, condition, traj,
                                                   budget, r);
          if (!row.ok()) return row.status();
          out << litert::lm::bench::CliffRowToJsonl(*row) << "\n";
          ++emitted;
        }
      }
    }
  }
  out.flush();
  std::cerr << "wrote " << emitted << " rows to " << output_path << "\n";
  return absl::OkStatus();
}

}  // namespace

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  auto status = RunMain();
  if (!status.ok()) {
    std::cerr << status << "\n";
    return 1;
  }
  return 0;
}
