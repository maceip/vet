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

#include "runtime/dpm/correction_protocol.h"

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "nlohmann/json.hpp"  // from @nlohmann_json
#include "runtime/dpm/event.h"
#include "runtime/dpm/event_sourced_log.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {
namespace {

bool IsZeroHash(const Hash256& hash) {
  static const Hash256 kZero;
  return hash == kZero;
}

std::string HashToHex(const Hash256& hash) { return hash.ToHex(); }

absl::StatusOr<Hash256> HashFromJson(const nlohmann::ordered_json& json,
                                     absl::string_view field) {
  if (!json.contains(std::string(field)) ||
      !json[std::string(field)].is_string()) {
    return absl::InvalidArgumentError(
        absl::StrCat("correction payload missing string field ", field));
  }
  bool ok = false;
  Hash256 hash =
      Hash256::FromHex(json[std::string(field)].get<std::string>(), &ok);
  if (!ok) {
    return absl::InvalidArgumentError(
        absl::StrCat("correction payload field ", field,
                     " is not a 32-byte hex hash."));
  }
  return hash;
}

bool MentionsCheckpoint(const CorrectionPayload& correction,
                        const Hash256& checkpoint_manifest_hash) {
  if (correction.target_checkpoint_manifest_hash ==
      checkpoint_manifest_hash) {
    return true;
  }
  for (const Hash256& invalidated : correction.invalidates_checkpoints) {
    if (invalidated == checkpoint_manifest_hash) return true;
  }
  return false;
}

}  // namespace

absl::string_view CorrectionSeverityToString(CorrectionSeverity severity) {
  switch (severity) {
    case CorrectionSeverity::kInfo:
      return "info";
    case CorrectionSeverity::kWarning:
      return "warning";
    case CorrectionSeverity::kBlocking:
      return "blocking";
  }
  return "blocking";
}

absl::StatusOr<CorrectionSeverity> CorrectionSeverityFromString(
    absl::string_view severity) {
  if (severity == "info") return CorrectionSeverity::kInfo;
  if (severity == "warning") return CorrectionSeverity::kWarning;
  if (severity == "blocking") return CorrectionSeverity::kBlocking;
  return absl::InvalidArgumentError(
      absl::StrCat("unknown correction severity: ", severity));
}

absl::Status ValidateCorrectionPayload(const CorrectionPayload& payload) {
  if (payload.correction_id.empty()) {
    return absl::InvalidArgumentError(
        "CorrectionPayload requires correction_id.");
  }
  if (IsZeroHash(payload.target_checkpoint_manifest_hash)) {
    return absl::InvalidArgumentError(
        "CorrectionPayload requires target_checkpoint_manifest_hash.");
  }
  if (payload.target_event_range_end < payload.target_event_range_start) {
    return absl::InvalidArgumentError(
        "CorrectionPayload event range is inverted.");
  }
  if (IsZeroHash(payload.audit_certificate_id)) {
    return absl::InvalidArgumentError(
        "CorrectionPayload requires audit_certificate_id.");
  }
  if (payload.reason_code.empty()) {
    return absl::InvalidArgumentError(
        "CorrectionPayload requires reason_code.");
  }
  if (payload.created_unix_micros <= 0) {
    return absl::InvalidArgumentError(
        "CorrectionPayload requires created_unix_micros.");
  }
  return absl::OkStatus();
}

std::string CorrectionPayloadToJson(const CorrectionPayload& payload) {
  nlohmann::ordered_json json = nlohmann::ordered_json::object();
  json["correction_id"] = payload.correction_id;
  json["target_checkpoint_manifest_hash"] =
      HashToHex(payload.target_checkpoint_manifest_hash);
  json["target_event_range_start"] = payload.target_event_range_start;
  json["target_event_range_end"] = payload.target_event_range_end;
  json["audit_certificate_id"] = HashToHex(payload.audit_certificate_id);
  json["reason_code"] = payload.reason_code;
  json["severity"] = std::string(CorrectionSeverityToString(payload.severity));
  json["drift_fields"] = payload.drift_fields;
  std::vector<std::string> invalidates;
  invalidates.reserve(payload.invalidates_checkpoints.size());
  for (const Hash256& hash : payload.invalidates_checkpoints) {
    invalidates.push_back(hash.ToHex());
  }
  json["invalidates_checkpoints"] = invalidates;
  json["replacement_projection"] = payload.replacement_projection;
  json["must_interrupt_before_next_predict"] =
      payload.must_interrupt_before_next_predict;
  json["created_unix_micros"] = payload.created_unix_micros;
  return json.dump();
}

absl::StatusOr<CorrectionPayload> CorrectionPayloadFromJson(
    absl::string_view text) {
  try {
    nlohmann::ordered_json json =
        nlohmann::ordered_json::parse(std::string(text));
    if (!json.is_object()) {
      return absl::InvalidArgumentError(
          "CorrectionPayload JSON must be an object.");
    }
    CorrectionPayload payload;
    payload.correction_id = json.value("correction_id", std::string());
    ASSIGN_OR_RETURN(payload.target_checkpoint_manifest_hash,
                     HashFromJson(json, "target_checkpoint_manifest_hash"));
    payload.target_event_range_start =
        json.value("target_event_range_start", uint64_t{0});
    payload.target_event_range_end =
        json.value("target_event_range_end", uint64_t{0});
    ASSIGN_OR_RETURN(payload.audit_certificate_id,
                     HashFromJson(json, "audit_certificate_id"));
    payload.reason_code = json.value("reason_code", std::string());
    ASSIGN_OR_RETURN(
        payload.severity,
        CorrectionSeverityFromString(json.value("severity", "blocking")));
    if (json.contains("drift_fields") && json["drift_fields"].is_array()) {
      payload.drift_fields = json["drift_fields"].get<std::vector<std::string>>();
    }
    if (json.contains("invalidates_checkpoints") &&
        json["invalidates_checkpoints"].is_array()) {
      for (const auto& item : json["invalidates_checkpoints"]) {
        if (!item.is_string()) {
          return absl::InvalidArgumentError(
              "invalidates_checkpoints entries must be strings.");
        }
        bool ok = false;
        Hash256 hash = Hash256::FromHex(item.get<std::string>(), &ok);
        if (!ok) {
          return absl::InvalidArgumentError(
              "invalidates_checkpoints entry is not a hash.");
        }
        payload.invalidates_checkpoints.push_back(hash);
      }
    }
    payload.replacement_projection =
        json.value("replacement_projection", std::string());
    payload.must_interrupt_before_next_predict =
        json.value("must_interrupt_before_next_predict", true);
    payload.created_unix_micros =
        json.value("created_unix_micros", int64_t{0});
    RETURN_IF_ERROR(ValidateCorrectionPayload(payload));
    return payload;
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid CorrectionPayload JSON: ", e.what()));
  }
}

absl::Status AppendCorrectionEvent(EventSourcedLog* log,
                                   const CorrectionPayload& payload) {
  if (log == nullptr) {
    return absl::InvalidArgumentError("EventSourcedLog is required.");
  }
  RETURN_IF_ERROR(ValidateCorrectionPayload(payload));
  return log->Append(Event{
      .type = Event::Type::kCorrection,
      .payload = CorrectionPayloadToJson(payload),
      .timestamp_us = payload.created_unix_micros,
  });
}

absl::StatusOr<CorrectionIndex> CorrectionIndex::Build(
    const std::vector<Event>& events) {
  CorrectionIndex index;
  for (const Event& event : events) {
    if (event.type != Event::Type::kCorrection) continue;
    ASSIGN_OR_RETURN(CorrectionPayload payload,
                     CorrectionPayloadFromJson(event.payload));
    index.corrections_.push_back(std::move(payload));
  }
  return index;
}

bool CorrectionIndex::HasBlockingCorrectionFor(
    const Hash256& checkpoint_manifest_hash) const {
  for (const CorrectionPayload& correction : corrections_) {
    if (correction.severity == CorrectionSeverity::kBlocking &&
        MentionsCheckpoint(correction, checkpoint_manifest_hash)) {
      return true;
    }
  }
  return false;
}

std::vector<CorrectionPayload> CorrectionIndex::BlockingCorrectionsFor(
    const Hash256& checkpoint_manifest_hash) const {
  std::vector<CorrectionPayload> out;
  for (const CorrectionPayload& correction : corrections_) {
    if (correction.severity == CorrectionSeverity::kBlocking &&
        MentionsCheckpoint(correction, checkpoint_manifest_hash)) {
      out.push_back(correction);
    }
  }
  return out;
}

}  // namespace litert::lm
