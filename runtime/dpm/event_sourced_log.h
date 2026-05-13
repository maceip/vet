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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_EVENT_SOURCED_LOG_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_EVENT_SOURCED_LOG_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/dpm/event.h"

namespace litert::lm {

class EventSink;
class PosixEventSink;

struct DPMLogIdentity {
  std::string tenant_id;
  std::string session_id;
};

// Decoded-event facade over an EventSink. EventSourcedLog owns the JSON
// serialization, per-record identity validation, and the in-memory cache;
// the sink owns durability, on-disk framing, and inter-process locking.
class EventSourcedLog {
 public:
  // Backwards-compatible constructor: builds an owned PosixEventSink rooted at
  // root_path and writes records under root_path/tenant_id/session_id/.
  EventSourcedLog(std::filesystem::path root_path, DPMLogIdentity identity);

  // Substrate-injected constructor: lets callers supply any EventSink (e.g.
  // a future S3 Files-aware sink, an in-memory test fake, or a Phase 3
  // Kinesis-backed sink). Ownership of the sink stays with the caller.
  EventSourcedLog(EventSink* sink, DPMLogIdentity identity);
  ~EventSourcedLog();

  const DPMLogIdentity& identity() const { return identity_; }

  // Returns the on-disk path when the log is backed by a PosixEventSink;
  // empty otherwise.
  std::filesystem::path path() const;

  absl::Status Append(Event event);
  absl::StatusOr<std::vector<Event>> GetAllEvents() const;
  absl::StatusOr<std::vector<Event>> GetEventRange(
      uint64_t event_range_start, uint64_t event_range_end) const;
  absl::StatusOr<std::string> GetProjectionEventLog() const;
  absl::StatusOr<std::string> GetProjectionEventLogRange(
      uint64_t event_range_start, uint64_t event_range_end) const;
  absl::StatusOr<std::vector<Event>> GetEventsSince(
      absl::string_view checkpoint) const;

 private:
  absl::Status ValidateIdentity() const;
  absl::Status ValidateEventIdentity(const Event& event) const;
  absl::StatusOr<std::vector<Event>> LoadEventsLocked() const;

  std::unique_ptr<PosixEventSink> owned_sink_;
  EventSink* sink_;
  DPMLogIdentity identity_;
  mutable std::mutex mutex_;
  mutable bool cache_loaded_ = false;
  // For probe-backed sinks this stores EventSink::Generation::record_count;
  // otherwise it stores the parsed record count.
  mutable uint64_t cached_record_count_ = 0;
  // For probe-backed sinks this stores EventSink::Generation::opaque_token;
  // otherwise it stores a content fingerprint of the records.
  mutable uint64_t cached_records_fingerprint_ = 0;
  mutable std::vector<Event> cached_events_;
  mutable bool projection_cache_loaded_ = false;
  // Same cache-key convention as cached_record_count_.
  mutable uint64_t cached_projection_record_count_ = 0;
  // Same cache-key convention as cached_records_fingerprint_.
  mutable uint64_t cached_projection_records_fingerprint_ = 0;
  mutable std::string cached_projection_event_log_;
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_EVENT_SOURCED_LOG_H_
