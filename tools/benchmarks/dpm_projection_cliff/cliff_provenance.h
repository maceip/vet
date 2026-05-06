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

#ifndef THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_CLIFF_PROVENANCE_H_
#define THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_CLIFF_PROVENANCE_H_

#include <optional>
#include <string>

namespace litert::lm::bench {

// Provenance fields populated from the host environment. These map
// 1-1 to the CliffRow provenance section. Each helper is best-effort:
// fields the host cannot supply are returned empty so the JSONL
// writer omits them rather than writing literal "unknown".
struct CliffProvenance {
  std::string git_sha;
  std::optional<bool> dirty_tree;
  std::string runtime_version;
  std::string config_hash;
  std::string hostname;
  std::string os;
  std::string cpu_model;
  std::string accelerator_id;
  std::string architecture_tag;
  std::string model_artifact_hash;  // BLAKE3-256 hex of the model bundle.
};

// Captures every field at once. `model_path` is hashed with BLAKE3 to
// produce model_artifact_hash; pass empty to skip. `config_path` is
// hashed similarly into config_hash. The runtime_version baked in at
// build time falls back to "litert-lm-bench" when unset.
CliffProvenance CaptureProvenance(const std::string& model_path,
                                  const std::string& config_path);

// Standalone helpers exposed for testing.
std::string DetectGitSha();          // 40-char SHA, empty on failure.
std::optional<bool> DetectDirtyTree();
std::string DetectHostname();
std::string DetectOs();              // "Linux 6.x.y x86_64", etc.
std::string DetectCpuModel();        // First "model name" in /proc/cpuinfo.
std::string DetectAcceleratorId();   // nvidia-smi -L line 0, or empty.
std::string DetectArchitectureTag(); // "linux_x86_64" / "linux_arm64" / ...
std::string Blake3HexOfFile(const std::string& path);

}  // namespace litert::lm::bench

#endif  // THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_CLIFF_PROVENANCE_H_
