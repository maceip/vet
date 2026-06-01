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

#include "tools/vet/vet_aid.h"

#include <fstream>

#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "nlohmann/json.hpp"  // from @nlohmann_json
#include "runtime/platform/hash/hasher.h"

namespace litert::lm::vet {
namespace {

std::string DigestHex(absl::string_view data) {
  return HashBytes(HashAlgorithm::kBlake3, data).ToHex();
}

}  // namespace

SessionPaths MakeSessionPaths(absl::string_view root, absl::string_view tenant,
                              absl::string_view session) {
  SessionPaths paths;
  paths.root = std::filesystem::path(std::string(root));
  paths.tenant_id = std::string(tenant);
  paths.session_id = std::string(session);
  paths.session_dir = paths.root / paths.tenant_id / paths.session_id;
  paths.aid_path = paths.session_dir / kAidFileName;
  paths.log_path = paths.session_dir / "events.dpmlog";
  return paths;
}

std::vector<std::string> DefaultAuthorizedTools() {
  return {
      "Bash",       "Read",        "Write",       "Edit",      "MultiEdit",
      "Glob",       "Grep",        "Task",        "WebFetch",  "WebSearch",
      "NotebookEdit",
  };
}

nlohmann::ordered_json BuildDefaultAid(const SessionPaths& paths,
                                       absl::string_view model_id,
                                       int64_t created_unix_micros) {
  nlohmann::ordered_json aid = nlohmann::ordered_json::object();
  aid["aid_version"] = kAidVersion;
  aid["schema_id"] = kDefaultSchemaId;
  aid["agent_id"] =
      absl::StrCat(paths.tenant_id, "/", paths.session_id);
  aid["tenant_id"] = paths.tenant_id;
  aid["session_id"] = paths.session_id;
  aid["created_unix_micros"] = created_unix_micros;
  aid["framework"] = "vet_sidecar";
  aid["reference"] =
      "VET sidecar: bind handoffs to append-only session logs and an Agent "
      "Identity Document (AID).";

  nlohmann::ordered_json core = nlohmann::ordered_json::object();
  core["mode"] = "coding_agent_sidecar";
  if (!model_id.empty()) {
    core["model_id"] = std::string(model_id);
  }
  core["authorized_tools"] = DefaultAuthorizedTools();
  aid["core"] = std::move(core);

  nlohmann::ordered_json components = nlohmann::ordered_json::array();
  components.push_back({
      {"id", "event_log"},
      {"kind", "local_trace"},
      {"proof_type", "trace_digest"},
      {"path", "events.dpmlog"},
  });
  components.push_back({
      {"id", "projection"},
      {"kind", "local_projection"},
      {"proof_type", "correction_aware_projection"},
      {"schema_id", kDefaultSchemaId},
  });
  aid["components"] = std::move(components);

  aid["claims"] = {
      {"verifies",
       nlohmann::ordered_json::array({"session_identity", "trace_digest",
                                      "correction_aware_handoff"})},
      {"does_not_verify",
       nlohmann::ordered_json::array({"host_orchestration", "llm_api_calls",
                                      "tool_http_transcripts"})},
  };
  aid["trust_assumptions"] = {
      {"host", "untrusted_for_memory_replay"},
      {"verifier", "honest_but_curious"},
  };
  return aid;
}

absl::Status WriteAidFile(const SessionPaths& paths,
                          const nlohmann::ordered_json& aid) {
  absl::Status validation = ValidateAidDocument(aid);
  if (!validation.ok()) return validation;
  std::error_code error;
  std::filesystem::create_directories(paths.session_dir, error);
  if (error) {
    return absl::InternalError(
        absl::StrCat("failed to create session directory: ", error.message()));
  }
  const std::string serialized = aid.dump(2);
  std::ofstream output(paths.aid_path, std::ios::out | std::ios::binary);
  if (!output.is_open()) {
    return absl::InternalError(
        absl::StrCat("cannot write AID file: ", paths.aid_path.string()));
  }
  output << serialized;
  if (!output.good()) {
    return absl::InternalError(
        absl::StrCat("failed while writing AID file: ", paths.aid_path.string()));
  }
  return absl::OkStatus();
}

absl::StatusOr<nlohmann::ordered_json> ReadAidFile(const SessionPaths& paths) {
  std::ifstream input(paths.aid_path, std::ios::in | std::ios::binary);
  if (!input.is_open()) {
    return absl::NotFoundError(
        absl::StrCat("AID file not found: ", paths.aid_path.string()));
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  try {
    nlohmann::ordered_json aid =
        nlohmann::ordered_json::parse(buffer.str());
    absl::Status validation = ValidateAidDocument(aid);
    if (!validation.ok()) return validation;
    return aid;
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(
        absl::StrCat("invalid AID JSON: ", e.what()));
  }
}

absl::Status ValidateAidDocument(const nlohmann::ordered_json& aid) {
  if (!aid.is_object()) {
    return absl::InvalidArgumentError("AID must be a JSON object.");
  }
  const auto require_string = [&](absl::string_view key) -> absl::Status {
    const auto it = aid.find(std::string(key));
    if (it == aid.end() || !it->is_string()) {
      return absl::InvalidArgumentError(
          absl::StrCat("AID missing string field '", key, "'."));
    }
    return absl::OkStatus();
  };
  if (!aid.contains("aid_version") || !aid["aid_version"].is_number_integer()) {
    return absl::InvalidArgumentError("AID missing integer aid_version.");
  }
  if (aid["aid_version"].get<int>() != kAidVersion) {
    return absl::InvalidArgumentError(
        absl::StrCat("unsupported aid_version; expected ", kAidVersion, "."));
  }
  absl::Status status = require_string("schema_id");
  if (!status.ok()) return status;
  status = require_string("tenant_id");
  if (!status.ok()) return status;
  status = require_string("session_id");
  if (!status.ok()) return status;
  if (!aid.contains("components") || !aid["components"].is_array()) {
    return absl::InvalidArgumentError("AID missing components array.");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> ComputeAidDigest(
    const nlohmann::ordered_json& aid) {
  absl::Status validation = ValidateAidDocument(aid);
  if (!validation.ok()) return validation;
  return DigestHex(aid.dump());
}

}  // namespace litert::lm::vet
