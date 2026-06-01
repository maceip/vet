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

#include "tools/vet/vet_trace.h"

#include <fstream>
#include <sstream>

#include <filesystem>

#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "nlohmann/json.hpp"  // from @nlohmann_json
#include "runtime/dpm/event.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/platform/hash/hasher.h"

namespace litert::lm::vet {
namespace {

std::string DigestHex(absl::string_view data) {
  return HashBytes(HashAlgorithm::kBlake3, data).ToHex();
}

size_t CountCorrectionEvents(const std::vector<Event>& events) {
  size_t count = 0;
  for (const Event& event : events) {
    if (event.type == Event::Type::kCorrection) ++count;
  }
  return count;
}

absl::StatusOr<std::string> ReadFileBytes(absl::string_view path) {
  std::ifstream input(std::string(path), std::ios::in | std::ios::binary);
  if (!input.is_open()) {
    return absl::NotFoundError(absl::StrCat("cannot read file: ", path));
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

void AppendEventWindowJson(const std::vector<Event>& events, size_t max_events,
                           nlohmann::ordered_json* out) {
  const size_t start =
      max_events > 0 && max_events < events.size() ? events.size() - max_events
                                                   : 0;
  nlohmann::ordered_json window = nlohmann::ordered_json::array();
  for (size_t i = start; i < events.size(); ++i) {
    nlohmann::ordered_json item = nlohmann::ordered_json::object();
    item["index"] = i + 1;
    item["line"] = EventToJsonLine(events[i]);
    window.push_back(std::move(item));
  }
  *out = std::move(window);
}

nlohmann::ordered_json CorrectionsToJson(
    const std::vector<ProjectionCorrectionDirective>& directives) {
  nlohmann::ordered_json out = nlohmann::ordered_json::array();
  for (const ProjectionCorrectionDirective& directive : directives) {
    nlohmann::ordered_json item = nlohmann::ordered_json::object();
    item["correction_event_index"] = directive.correction_event_index + 1;
    item["correction_event_id"] = directive.correction_event_id;
    item["correction_text"] = directive.correction_text;
    item["invalidated_facts"] = directive.invalidated_facts;
    item["replacement_facts"] = directive.replacement_facts;
    item["scope"] = ProjectionCorrectionScopeToString(directive.scope);
    out.push_back(std::move(item));
  }
  return out;
}

}  // namespace

namespace {

void AppendFactList(absl::string_view label,
                    const std::vector<std::string>& facts,
                    std::string* out) {
  absl::StrAppend(out, "  ", label, ":\n");
  bool wrote_fact = false;
  for (const std::string& fact : facts) {
    if (fact.empty()) continue;
    absl::StrAppend(out, "    - ", fact, "\n");
    wrote_fact = true;
  }
  if (!wrote_fact) absl::StrAppend(out, "    - none declared\n");
}

std::string FormatCorrectionBlock(
    const std::vector<ProjectionCorrectionDirective>& directives) {
  if (directives.empty()) return "";
  std::string out =
      "[VET BLOCKING CORRECTIONS]\n"
      "Apply every correction below before producing active task memory. "
      "Suppress invalidated facts exactly; prefer replacement facts when "
      "provided; if a conflict remains, emit unknown instead of the old fact. "
      "Invalidated facts listed here are suppression targets, not active "
      "memory.\n";
  for (const ProjectionCorrectionDirective& directive : directives) {
    absl::StrAppend(
        &out, "- correction_id: ",
        directive.correction_event_id.empty() ? "unknown"
                                              : directive.correction_event_id,
        "\n",
        "  correction_event: [", directive.correction_event_index + 1, "]\n",
        "  scope: ", ProjectionCorrectionScopeToString(directive.scope), "\n");
    if (!directive.correction_text.empty()) {
      absl::StrAppend(&out, "  correction_text: ", directive.correction_text,
                      "\n");
    }
    AppendFactList("invalidated_facts", directive.invalidated_facts, &out);
    AppendFactList("replacement_facts", directive.replacement_facts, &out);
  }
  absl::StrAppend(
      &out,
      "Rules: do not include invalidated facts in Facts, Reasoning, "
      "Compliance, decisions, plans, release notes, or handoffs.\n\n");
  return out;
}

std::string RenderEventLogWindow(const std::vector<Event>& events,
                                 size_t max_events) {
  const size_t start =
      max_events > 0 && max_events < events.size() ? events.size() - max_events
                                                   : 0;
  std::string out;
  for (size_t i = start; i < events.size(); ++i) {
    if (!out.empty()) out.push_back('\n');
    absl::StrAppend(&out, "[", i + 1, "] ", EventToJsonLine(events[i]));
  }
  return out;
}

}  // namespace

ValidTraceReport ValidateExecutionTrace(const std::vector<Event>& events,
                                        const SessionPaths& paths) {
  ValidTraceReport report;
  report.valid = true;
  int64_t last_step = 0;
  int64_t last_timestamp = 0;
  std::vector<int64_t> seen_steps;
  seen_steps.reserve(events.size());

  for (size_t i = 0; i < events.size(); ++i) {
    const Event& event = events[i];
    if (event.tenant_id.empty()) {
      report.valid = false;
      report.errors.push_back(
          absl::StrCat("event ", i + 1, " missing tenant_id"));
    } else if (event.tenant_id != paths.tenant_id) {
      report.valid = false;
      report.errors.push_back(absl::StrCat(
          "event ", i + 1, " tenant_id mismatch: expected ", paths.tenant_id,
          ", got ", event.tenant_id));
    }
    if (event.session_id.empty()) {
      report.valid = false;
      report.errors.push_back(
          absl::StrCat("event ", i + 1, " missing session_id"));
    } else if (event.session_id != paths.session_id) {
      report.valid = false;
      report.errors.push_back(absl::StrCat(
          "event ", i + 1, " session_id mismatch: expected ", paths.session_id,
          ", got ", event.session_id));
    }
    if (event.timestamp_us < last_timestamp) {
      report.warnings.push_back(absl::StrCat(
          "event ", i + 1, " timestamp_us regressed (", event.timestamp_us,
          " < ", last_timestamp, ")"));
    }
    last_timestamp = event.timestamp_us;

    const int64_t step = event.step_index > 0
                             ? event.step_index
                             : static_cast<int64_t>(i + 1);
    if (step <= 0) {
      report.valid = false;
      report.errors.push_back(
          absl::StrCat("event ", i + 1, " has invalid step_index ", step));
    }
    if (step < last_step) {
      report.valid = false;
      report.errors.push_back(absl::StrCat(
          "event ", i + 1, " step_index regressed: ", step, " < ", last_step));
    }
    for (int64_t prior : seen_steps) {
      if (prior == step) {
        report.valid = false;
        report.errors.push_back(absl::StrCat(
            "event ", i + 1, " duplicate step_index ", step));
        break;
      }
    }
    seen_steps.push_back(step);
    last_step = step;

    if (event.type == Event::Type::kTool && event.tool_call_id.empty()) {
      report.warnings.push_back(absl::StrCat(
          "event ", i + 1, " tool event missing tool_call_id"));
    }
    if (event.type == Event::Type::kCorrection && event.payload.empty()) {
      report.warnings.push_back(absl::StrCat(
          "event ", i + 1, " correction event has empty payload"));
    }
  }
  return report;
}

absl::StatusOr<std::string> ComputeTraceDigestFromFile(
    absl::string_view log_path) {
  absl::StatusOr<std::string> bytes = ReadFileBytes(log_path);
  if (!bytes.ok()) return bytes.status();
  return DigestHex(*bytes);
}

void EnsureTraceCoordinates(std::vector<Event>* events) {
  if (events == nullptr) return;
  for (size_t i = 0; i < events->size(); ++i) {
    Event& event = (*events)[i];
    if (event.step_index <= 0) {
      event.step_index = static_cast<int64_t>(i + 1);
    }
    if (event.type == Event::Type::kTool && event.tool_call_id.empty()) {
      event.tool_call_id = absl::StrCat("tool-", event.step_index);
    }
  }
}

nlohmann::ordered_json BuildHandoffBundle(const HandoffBundleRequest& request) {
  nlohmann::ordered_json bundle = nlohmann::ordered_json::object();
  bundle["bundle_version"] = kHandoffBundleVersion;
  bundle["task"] = std::string(request.task);
  bundle["output_text"] = std::string(request.output_text);
  bundle["identity"] =
      absl::StrCat(request.paths.tenant_id, "/", request.paths.session_id);
  bundle["tenant_id"] = request.paths.tenant_id;
  bundle["session_id"] = request.paths.session_id;
  bundle["schema_id"] = request.aid.value("schema_id", kDefaultSchemaId);
  bundle["aid_path"] = kAidFileName;
  bundle["aid_digest"] = request.aid_digest;
  bundle["created_unix_micros"] = request.created_unix_micros;

  const std::vector<Event>& events =
      request.events != nullptr ? *request.events : std::vector<Event>{};
  const size_t event_count = events.size();
  bundle["trace"] = {
      {"event_count", event_count},
      {"event_range_start", event_count == 0 ? 0 : 1},
      {"event_range_end", event_count},
      {"log_path", "events.dpmlog"},
      {"trace_digest", request.trace_digest},
  };

  if (request.corrections != nullptr) {
    bundle["corrections"] = CorrectionsToJson(*request.corrections);
  } else {
    bundle["corrections"] = nlohmann::ordered_json::array();
  }

  AppendEventWindowJson(events, request.max_events, &bundle["recent_events"]);

  nlohmann::ordered_json proofs = nlohmann::ordered_json::array();
  proofs.push_back({
      {"component_id", "event_log"},
      {"proof_type", "trace_digest"},
      {"algorithm", "blake3"},
      {"digest", request.trace_digest},
  });
  proofs.push_back({
      {"component_id", "projection"},
      {"proof_type", "correction_aware_projection"},
      {"schema_id", request.aid.value("schema_id", kDefaultSchemaId)},
      {"correction_count", request.corrections != nullptr
                               ? request.corrections->size()
                               : 0},
  });
  bundle["component_proofs"] = std::move(proofs);
  return bundle;
}

absl::StatusOr<VerifyReport> VerifyHandoffBundle(
    const nlohmann::ordered_json& bundle, const SessionPaths& paths) {
  VerifyReport report;
  auto pass = [&](absl::string_view check) {
    report.checks_passed.emplace_back(std::string(check));
  };
  auto fail = [&](absl::string_view check, absl::string_view detail) {
    const std::string check_id(check);
    report.checks_failed.emplace_back(check_id);
    report.failure_details[check_id] = detail;
  };

  if (!bundle.is_object()) {
    fail("bundle_is_object", "handoff bundle must be a JSON object");
    report.verified = false;
    return report;
  }
  pass("bundle_is_object");

  if (!bundle.contains("bundle_version") ||
      !bundle["bundle_version"].is_number_integer()) {
    fail("bundle_version",
         absl::StrCat("missing or invalid bundle_version; expected ",
                      kHandoffBundleVersion));
  } else if (bundle["bundle_version"].get<int>() != kHandoffBundleVersion) {
    fail("bundle_version",
         absl::StrCat("unsupported bundle_version ",
                      bundle["bundle_version"].get<int>(), "; expected ",
                      kHandoffBundleVersion));
  } else {
    pass("bundle_version");
  }

  const std::string bundled_tenant = bundle.value("tenant_id", std::string());
  const std::string bundled_session = bundle.value("session_id", std::string());
  if (bundled_tenant != paths.tenant_id ||
      bundled_session != paths.session_id) {
    fail("identity_match",
         absl::StrCat("bundle identity ", bundled_tenant, "/",
                      bundled_session, " != session ", paths.tenant_id, "/",
                      paths.session_id));
  } else {
    pass("identity_match");
  }

  absl::StatusOr<nlohmann::ordered_json> aid = ReadAidFile(paths);
  if (!aid.ok()) {
    fail("aid_present", std::string(aid.status().message()));
    report.verified = false;
    return report;
  }
  pass("aid_present");

  absl::StatusOr<std::string> live_aid_digest = ComputeAidDigest(*aid);
  if (!live_aid_digest.ok()) {
    fail("aid_digest_compute", std::string(live_aid_digest.status().message()));
  } else {
    const std::string bundled_aid_digest =
        bundle.value("aid_digest", std::string());
    if (bundled_aid_digest != *live_aid_digest) {
      fail("aid_digest_match",
           absl::StrCat("bundle aid_digest ", bundled_aid_digest,
                        " != live aid_digest ", *live_aid_digest));
    } else {
      pass("aid_digest_match");
    }
  }

  absl::StatusOr<std::string> live_trace_digest =
      ComputeTraceDigestFromFile(paths.log_path.string());
  if (!live_trace_digest.ok()) {
    fail("trace_digest_compute",
         std::string(live_trace_digest.status().message()));
    report.verified = false;
    return report;
  }
  pass("trace_digest_compute");

  std::string bundled_trace_digest;
  if (bundle.contains("component_proofs") &&
      bundle["component_proofs"].is_array()) {
    for (const auto& proof : bundle["component_proofs"]) {
      if (proof.value("component_id", std::string()) == "event_log" &&
          proof.value("proof_type", std::string()) == "trace_digest") {
        bundled_trace_digest = proof.value("digest", std::string());
        break;
      }
    }
  }
  if (bundled_trace_digest.empty()) {
    bundled_trace_digest =
        bundle["trace"].value("trace_digest", std::string());
  }
  if (bundled_trace_digest.empty()) {
    fail("trace_digest_present", "bundle missing trace_digest");
  } else if (bundled_trace_digest != *live_trace_digest) {
    fail("trace_digest_match",
         absl::StrCat("bundle trace_digest ", bundled_trace_digest,
                      " != live trace_digest ", *live_trace_digest));
  } else {
    pass("trace_digest_match");
  }

  EventSourcedLog log(
      paths.root,
      DPMLogIdentity{.tenant_id = paths.tenant_id,
                     .session_id = paths.session_id});
  if (log.path().empty() || !std::filesystem::exists(log.path())) {
    fail("event_log_readable",
         absl::StrCat("event log not found at ", paths.log_path.string()));
    report.verified = false;
    return report;
  }
  pass("event_log_readable");

  absl::StatusOr<std::vector<Event>> loaded = log.GetAllEvents();
  if (!loaded.ok()) {
    fail("event_log_parse", std::string(loaded.status().message()));
    report.verified = false;
    report.trace_report.valid = false;
    report.trace_report.errors.push_back(std::string(loaded.status().message()));
    return report;
  }
  const std::vector<Event>& events = *loaded;
  pass("event_log_parse");

  const uint64_t bundled_count =
      bundle["trace"].value("event_count", static_cast<uint64_t>(0));
  if (bundled_count != events.size()) {
    fail("event_count_match",
         absl::StrCat("bundle event_count ", bundled_count,
                      " != live event_count ", events.size()));
  } else {
    pass("event_count_match");
  }

  const uint64_t range_start =
      bundle["trace"].value("event_range_start", static_cast<uint64_t>(0));
  const uint64_t range_end =
      bundle["trace"].value("event_range_end", static_cast<uint64_t>(0));
  if (events.empty()) {
    if (range_start != 0 || range_end != 0) {
      fail("event_range_match",
           absl::StrCat("empty log but bundle event_range ", range_start, "-",
                        range_end));
    } else {
      pass("event_range_match");
    }
  } else if (range_start != 1 || range_end != events.size()) {
    fail("event_range_match",
         absl::StrCat("bundle event_range ", range_start, "-", range_end,
                      " != expected 1-", events.size()));
  } else {
    pass("event_range_match");
  }

  const size_t live_correction_count = CountCorrectionEvents(events);
  size_t bundled_correction_count = 0;
  if (bundle.contains("corrections") && bundle["corrections"].is_array()) {
    bundled_correction_count = bundle["corrections"].size();
  }
  if (bundled_correction_count != live_correction_count) {
    fail("correction_count_match",
         absl::StrCat("bundle corrections ", bundled_correction_count,
                      " != live correction events ", live_correction_count));
  } else {
    pass("correction_count_match");
  }

  size_t proof_correction_count = 0;
  bool found_projection_proof = false;
  if (bundle.contains("component_proofs") &&
      bundle["component_proofs"].is_array()) {
    for (const auto& proof : bundle["component_proofs"]) {
      if (proof.value("component_id", std::string()) == "projection" &&
          proof.value("proof_type", std::string()) ==
              "correction_aware_projection") {
        found_projection_proof = true;
        proof_correction_count =
            proof.value("correction_count", static_cast<size_t>(0));
        break;
      }
    }
  }
  if (!found_projection_proof) {
    fail("projection_proof_present",
         "bundle missing projection component proof");
  } else if (proof_correction_count != live_correction_count) {
    fail("projection_correction_count_match",
         absl::StrCat("projection proof correction_count ",
                      proof_correction_count, " != live correction events ",
                      live_correction_count));
  } else {
    pass("projection_correction_count_match");
  }

  report.trace_report = ValidateExecutionTrace(events, paths);
  if (report.trace_report.valid) {
    pass("valid_trace");
  } else {
    const std::string detail =
        report.trace_report.errors.empty()
            ? "execution trace failed structural validation"
            : report.trace_report.errors.front();
    fail("valid_trace", detail);
  }

  report.verified = report.checks_failed.empty();
  return report;
}

absl::StatusOr<nlohmann::ordered_json> ParseJsonInput(
    absl::string_view json_text) {
  try {
    return nlohmann::ordered_json::parse(std::string(json_text));
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(
        absl::StrCat("invalid JSON input: ", e.what()));
  }
}

std::string BuildHandoffText(
    absl::string_view task, const SessionPaths& paths,
    const std::vector<Event>& events,
    const std::vector<ProjectionCorrectionDirective>& directives,
    size_t max_events) {
  std::string out;
  absl::StrAppend(&out, "[VET HANDOFF]\n", "task: ", task, "\n",
                  "identity: ", paths.tenant_id, "/", paths.session_id, "\n",
                  "events_total: ", events.size(), "\n",
                  "aid: ", kAidFileName, "\n\n", "[POLICY]\n",
                  "- Treat correction events and blocking corrections as "
                  "superseding conflicting older facts.\n",
                  "- Do not carry invalidated facts into the active plan, "
                  "answer, or future handoff.\n",
                  "- Invalidated facts may be quoted below only as correction "
                  "metadata or audit log content.\n",
                  "- Cite event indices when relying on remembered context.\n",
                  "- Use `vet verify` on JSON bundles before trusting host "
                  "handoffs.\n\n");
  const std::string correction_block = FormatCorrectionBlock(directives);
  if (!correction_block.empty()) absl::StrAppend(&out, correction_block);
  absl::StrAppend(&out, "[RECENT EVENT LOG - AUDIT ONLY]\n",
                  RenderEventLogWindow(events, max_events), "\n");
  return out;
}

}  // namespace litert::lm::vet
