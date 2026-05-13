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

#include "runtime/dpm/event_sourced_log.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "absl/hash/hash.h"  // from @com_google_absl
#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/dpm/event.h"
#include "runtime/platform/eventlog/event_sink.h"
#include "runtime/platform/eventlog/posix_event_sink.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {
namespace {

bool IsValidIdentityComponent(absl::string_view value) {
  return !value.empty() && value != "." && value != ".." &&
         value.find('/') == absl::string_view::npos &&
         value.find('\\') == absl::string_view::npos;
}

}  // namespace

EventSourcedLog::EventSourcedLog(std::filesystem::path root_path,
                                 DPMLogIdentity identity)
    : owned_sink_(std::make_unique<PosixEventSink>(std::move(root_path))),
      sink_(owned_sink_.get()),
      identity_(std::move(identity)) {}

EventSourcedLog::EventSourcedLog(EventSink* sink, DPMLogIdentity identity)
    : owned_sink_(nullptr), sink_(sink), identity_(std::move(identity)) {}

EventSourcedLog::~EventSourcedLog() = default;

std::filesystem::path EventSourcedLog::path() const {
  if (auto* posix = dynamic_cast<PosixEventSink*>(sink_)) {
    return posix->PathFor(identity_.tenant_id, identity_.session_id);
  }
  return {};
}

absl::Status EventSourcedLog::ValidateIdentity() const {
  if (!IsValidIdentityComponent(identity_.tenant_id)) {
    return absl::InvalidArgumentError(
        "DPM tenant_id must be non-empty and must not contain path separators.");
  }
  if (!IsValidIdentityComponent(identity_.session_id)) {
    return absl::InvalidArgumentError(
        "DPM session_id must be non-empty and must not contain path "
        "separators.");
  }
  return absl::OkStatus();
}

absl::Status EventSourcedLog::ValidateEventIdentity(const Event& event) const {
  if (event.tenant_id != identity_.tenant_id ||
      event.session_id != identity_.session_id) {
    return absl::InvalidArgumentError(
        "DPM event identity does not match the owning EventSourcedLog.");
  }
  return absl::OkStatus();
}

absl::Status EventSourcedLog::Append(Event event) {
  if (sink_ == nullptr) {
    return absl::FailedPreconditionError(
        "DPM EventSourcedLog has no event sink bound.");
  }
  RETURN_IF_ERROR(ValidateIdentity());
  if (event.tenant_id.empty()) {
    event.tenant_id = identity_.tenant_id;
  }
  if (event.session_id.empty()) {
    event.session_id = identity_.session_id;
  }
  RETURN_IF_ERROR(ValidateEventIdentity(event));

  std::lock_guard<std::mutex> lock(mutex_);
  const std::string payload = EventToJsonLine(event);
  RETURN_IF_ERROR(sink_->AppendRecord(identity_.tenant_id,
                                      identity_.session_id, payload));
  cache_loaded_ = false;
  cached_record_count_ = 0;
  cached_records_fingerprint_ = 0;
  cached_events_.clear();
  projection_cache_loaded_ = false;
  cached_projection_record_count_ = 0;
  cached_projection_records_fingerprint_ = 0;
  cached_projection_event_log_.clear();
  return absl::OkStatus();
}

absl::StatusOr<std::vector<Event>> EventSourcedLog::LoadEventsLocked() const {
  // Fast path: ask the sink for a cheap generation token. If the cache is
  // populated and the token matches, skip the parse entirely.
  absl::StatusOr<EventSink::Generation> probe =
      sink_->ProbeGeneration(identity_.tenant_id, identity_.session_id);
  if (probe.ok() && cache_loaded_ &&
      cached_records_fingerprint_ == probe->opaque_token &&
      cached_record_count_ == probe->record_count) {
    return cached_events_;
  }
  if (!probe.ok() &&
      probe.status().code() != absl::StatusCode::kUnimplemented) {
    return probe.status();
  }

  std::vector<Event> events;
  uint64_t record_count = 0;
  uint64_t records_fingerprint = 0;
  RETURN_IF_ERROR(sink_->ForEachRecord(
      identity_.tenant_id, identity_.session_id,
      [&](absl::string_view record) -> absl::Status {
        records_fingerprint =
            absl::HashOf(records_fingerprint, record, record_count);
        absl::StatusOr<Event> event = EventFromJsonLine(record);
        if (!event.ok()) {
          return absl::DataLossError(
              absl::StrCat("DPM event log record ", record_count,
                           " failed to parse: ", event.status().message()));
        }
        if (event->tenant_id != identity_.tenant_id ||
            event->session_id != identity_.session_id) {
          return absl::DataLossError(
              "DPM event log contains a cross-tenant or cross-session event.");
        }
        events.push_back(std::move(*event));
        ++record_count;
        return absl::OkStatus();
      }));
  if (!probe.ok() && cache_loaded_ && cached_record_count_ == record_count &&
      cached_records_fingerprint_ == records_fingerprint) {
    return cached_events_;
  }
  cached_events_ = events;
  cached_record_count_ = probe.ok() ? probe->record_count : record_count;
  // Prefer the sink's opaque_token as the cache key when available; fall back
  // to the per-record content fingerprint so backends without a probe still
  // get content-addressed invalidation.
  cached_records_fingerprint_ =
      probe.ok() ? probe->opaque_token : records_fingerprint;
  cache_loaded_ = true;
  return cached_events_;
}

absl::StatusOr<std::vector<Event>> EventSourcedLog::GetAllEvents() const {
  if (sink_ == nullptr) {
    return absl::FailedPreconditionError(
        "DPM EventSourcedLog has no event sink bound.");
  }
  RETURN_IF_ERROR(ValidateIdentity());
  std::lock_guard<std::mutex> lock(mutex_);
  return LoadEventsLocked();
}

absl::StatusOr<std::vector<Event>> EventSourcedLog::GetEventRange(
    uint64_t event_range_start, uint64_t event_range_end) const {
  if (event_range_end < event_range_start) {
    return absl::InvalidArgumentError("DPM event range is inverted.");
  }
  if (sink_ == nullptr) {
    return absl::FailedPreconditionError(
        "DPM EventSourcedLog has no event sink bound.");
  }
  RETURN_IF_ERROR(ValidateIdentity());
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Event> events;
  events.reserve(static_cast<size_t>(event_range_end - event_range_start));
  uint64_t observed = event_range_start;
  RETURN_IF_ERROR(sink_->ForEachRecordRange(
      identity_.tenant_id, identity_.session_id, event_range_start,
      event_range_end, [&](absl::string_view record) -> absl::Status {
        absl::StatusOr<Event> event = EventFromJsonLine(record);
        if (!event.ok()) {
          return absl::DataLossError(absl::StrCat(
              "DPM event log record ", observed, " failed to parse: ",
              event.status().message()));
        }
        if (event->tenant_id != identity_.tenant_id ||
            event->session_id != identity_.session_id) {
          return absl::DataLossError(
              "DPM event log contains a cross-tenant or cross-session event.");
        }
        events.push_back(std::move(*event));
        ++observed;
        return absl::OkStatus();
      }));
  if (events.size() != event_range_end - event_range_start) {
    return absl::InvalidArgumentError(
        "DPM event range exceeds log generation.");
  }
  return events;
}

absl::StatusOr<std::string> EventSourcedLog::GetProjectionEventLog() const {
  if (sink_ == nullptr) {
    return absl::FailedPreconditionError(
        "DPM EventSourcedLog has no event sink bound.");
  }
  RETURN_IF_ERROR(ValidateIdentity());
  std::lock_guard<std::mutex> lock(mutex_);

  absl::StatusOr<EventSink::Generation> probe =
      sink_->ProbeGeneration(identity_.tenant_id, identity_.session_id);
  if (probe.ok() && projection_cache_loaded_ &&
      cached_projection_records_fingerprint_ == probe->opaque_token &&
      cached_projection_record_count_ == probe->record_count) {
    return cached_projection_event_log_;
  }
  if (!probe.ok() &&
      probe.status().code() != absl::StatusCode::kUnimplemented) {
    return probe.status();
  }

  std::string event_log;
  uint64_t record_count = 0;
  uint64_t records_fingerprint = 0;
  RETURN_IF_ERROR(sink_->ForEachRecord(
      identity_.tenant_id, identity_.session_id,
      [&](absl::string_view record) -> absl::Status {
        records_fingerprint =
            absl::HashOf(records_fingerprint, record, record_count);
        absl::StatusOr<Event> event = EventFromJsonLine(record);
        if (!event.ok()) {
          return absl::DataLossError(
              absl::StrCat("DPM event log record ", record_count,
                           " failed to parse: ", event.status().message()));
        }
        if (event->tenant_id != identity_.tenant_id ||
            event->session_id != identity_.session_id) {
          return absl::DataLossError(
              "DPM event log contains a cross-tenant or cross-session event.");
        }
        if (record_count > 0) {
          event_log.push_back('\n');
        }
        absl::StrAppend(&event_log, "[", record_count + 1, "] ");
        event_log.append(record.data(), record.size());
        ++record_count;
        return absl::OkStatus();
      }));
  if (!probe.ok() && projection_cache_loaded_ &&
      cached_projection_record_count_ == record_count &&
      cached_projection_records_fingerprint_ == records_fingerprint) {
    return cached_projection_event_log_;
  }

  cached_projection_event_log_ = std::move(event_log);
  cached_projection_record_count_ =
      probe.ok() ? probe->record_count : record_count;
  cached_projection_records_fingerprint_ =
      probe.ok() ? probe->opaque_token : records_fingerprint;
  projection_cache_loaded_ = true;
  return cached_projection_event_log_;
}

absl::StatusOr<std::string> EventSourcedLog::GetProjectionEventLogRange(
    uint64_t event_range_start, uint64_t event_range_end) const {
  if (event_range_end < event_range_start) {
    return absl::InvalidArgumentError("DPM projection event range is inverted.");
  }
  if (sink_ == nullptr) {
    return absl::FailedPreconditionError(
        "DPM EventSourcedLog has no event sink bound.");
  }
  RETURN_IF_ERROR(ValidateIdentity());
  std::lock_guard<std::mutex> lock(mutex_);
  std::string event_log;
  uint64_t observed = event_range_start;
  RETURN_IF_ERROR(sink_->ForEachRecordRange(
      identity_.tenant_id, identity_.session_id, event_range_start,
      event_range_end, [&](absl::string_view record) -> absl::Status {
        absl::StatusOr<Event> event = EventFromJsonLine(record);
        if (!event.ok()) {
          return absl::DataLossError(absl::StrCat(
              "DPM event log record ", observed, " failed to parse: ",
              event.status().message()));
        }
        if (event->tenant_id != identity_.tenant_id ||
            event->session_id != identity_.session_id) {
          return absl::DataLossError(
              "DPM event log contains a cross-tenant or cross-session event.");
        }
        if (!event_log.empty()) event_log.push_back('\n');
        absl::StrAppend(&event_log, "[", observed + 1, "] ", record);
        ++observed;
        return absl::OkStatus();
      }));
  if (observed != event_range_end) {
    return absl::InvalidArgumentError(
        "DPM projection event range exceeds log generation.");
  }
  return event_log;
}

absl::StatusOr<std::vector<Event>> EventSourcedLog::GetEventsSince(
    absl::string_view checkpoint) const {
  int64_t checkpoint_index = 0;
  try {
    std::string checkpoint_string(checkpoint);
    size_t parsed_chars = 0;
    checkpoint_index = std::stoll(checkpoint_string, &parsed_chars);
    if (parsed_chars != checkpoint_string.size() || checkpoint_index < 0) {
      return absl::InvalidArgumentError(
          "DPM checkpoint must be a non-negative event index.");
    }
  } catch (const std::exception&) {
    return absl::InvalidArgumentError(
        "DPM checkpoint must be a non-negative event index.");
  }

  ASSIGN_OR_RETURN(std::vector<Event> events, GetAllEvents());
  const size_t checkpoint_offset = static_cast<size_t>(checkpoint_index);
  if (checkpoint_offset >= events.size()) {
    return std::vector<Event>();
  }
  return std::vector<Event>(events.begin() + checkpoint_offset, events.end());
}

}  // namespace litert::lm
