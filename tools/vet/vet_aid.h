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

#ifndef THIRD_PARTY_ODML_LITERT_LM_TOOLS_VET_VET_AID_H_
#define THIRD_PARTY_ODML_LITERT_LM_TOOLS_VET_VET_AID_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "nlohmann/json.hpp"  // from @nlohmann_json

namespace litert::lm::vet {

inline constexpr char kAidFileName[] = "aid.json";
inline constexpr char kDefaultSchemaId[] = "vet.agent_sidecar.v1";
inline constexpr int kAidVersion = 1;
inline constexpr int kHandoffBundleVersion = 1;

struct SessionPaths {
  std::filesystem::path root;
  std::string tenant_id;
  std::string session_id;
  std::filesystem::path session_dir;
  std::filesystem::path aid_path;
  std::filesystem::path log_path;
};

SessionPaths MakeSessionPaths(absl::string_view root, absl::string_view tenant,
                              absl::string_view session);

// Builds the default Agent Identity Document (AID) for a coding-agent session.
nlohmann::ordered_json BuildDefaultAid(const SessionPaths& paths,
                                       absl::string_view model_id,
                                       int64_t created_unix_micros);

absl::Status WriteAidFile(const SessionPaths& paths,
                          const nlohmann::ordered_json& aid);

absl::StatusOr<nlohmann::ordered_json> ReadAidFile(const SessionPaths& paths);

absl::Status ValidateAidDocument(const nlohmann::ordered_json& aid);

// BLAKE3 digest of the canonical on-disk aid.json bytes (or serialized JSON).
absl::StatusOr<std::string> ComputeAidDigest(
    const nlohmann::ordered_json& aid);

std::vector<std::string> DefaultAuthorizedTools();

}  // namespace litert::lm::vet

#endif  // THIRD_PARTY_ODML_LITERT_LM_TOOLS_VET_VET_AID_H_
