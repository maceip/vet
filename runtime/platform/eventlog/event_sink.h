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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_EVENTLOG_EVENT_SINK_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_EVENTLOG_EVENT_SINK_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/functional/function_ref.h"  // from @com_google_absl
#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl

namespace litert::lm {

// Substrate-level append-only record store. Implementations own the on-disk
// (or remote) framing, locking, and durability semantics. Higher layers
// (e.g. DPM's EventSourcedLog) supply opaque record bytes and consume them
// back in append order.
//
// AppendRecord must return only after the record is durable on the substrate.
class EventSink {
 public:
  virtual ~EventSink() = default;

  // Retention metadata applied alongside an append. Implementations that do
  // not support per-record retention may ignore the policy; substrates that
  // do (e.g. PosixEventSink writing a sidecar JSON) persist it next to the
  // record. Bucket-level Object Lock on the underlying S3 bucket continues
  // to enforce immutability for synced objects regardless of this metadata.
  struct RetentionPolicy {
    int64_t retain_until_unix_seconds = 0;
    bool legal_hold = false;
    bool empty() const {
      return retain_until_unix_seconds == 0 && !legal_hold;
    }
  };

  virtual absl::Status AppendRecord(absl::string_view tenant_id,
                                    absl::string_view session_id,
                                    absl::string_view record_payload) {
    return AppendRecordWithRetention(tenant_id, session_id, record_payload,
                                     RetentionPolicy{});
  }

  // Sinks that support retention metadata override this; the default
  // implementation falls back to the no-retention AppendRecord, so existing
  // sinks (test fakes, in-memory backends) keep working unchanged. Sinks
  // that do not support retention but receive a non-empty policy must return
  // an error rather than silently dropping it.
  virtual absl::Status AppendRecordWithRetention(
      absl::string_view tenant_id, absl::string_view session_id,
      absl::string_view record_payload, const RetentionPolicy& retention) {
    if (!retention.empty()) {
      return absl::UnimplementedError(
          "EventSink backend does not support retention policy.");
    }
    return AppendRecord(tenant_id, session_id, record_payload);
  }

  virtual absl::StatusOr<std::vector<std::string>> ReadRecords(
      absl::string_view tenant_id, absl::string_view session_id) const = 0;

  virtual absl::Status ForEachRecord(
      absl::string_view tenant_id, absl::string_view session_id,
      absl::FunctionRef<absl::Status(absl::string_view)> callback) const {
    absl::StatusOr<std::vector<std::string>> records =
        ReadRecords(tenant_id, session_id);
    if (!records.ok()) {
      return records.status();
    }
    for (const std::string& record : *records) {
      absl::Status status = callback(record);
      if (!status.ok()) {
        return status;
      }
    }
    return absl::OkStatus();
  }

  // Iterates records in the half-open global range [start, end). Backends
  // should stop reading once `end` is reached; the default implementation is
  // correctness-first and may scan the full stream.
  virtual absl::Status ForEachRecordRange(
      absl::string_view tenant_id, absl::string_view session_id,
      uint64_t start, uint64_t end,
      absl::FunctionRef<absl::Status(absl::string_view)> callback) const {
    if (end < start) {
      return absl::InvalidArgumentError("EventSink record range is inverted.");
    }
    uint64_t index = 0;
    return ForEachRecord(
        tenant_id, session_id,
        [&](absl::string_view record) -> absl::Status {
          if (index >= start && index < end) {
            absl::Status status = callback(record);
            if (!status.ok()) return status;
          }
          ++index;
          return absl::OkStatus();
        });
  }

  // Generation token that consumers can use to invalidate decoded-record
  // caches without re-reading the underlying records. Treat the pair as an
  // opaque cache key: implementations may populate record_count when it is
  // cheap, or leave it at zero and advance opaque_token instead.
  // The default implementation returns kUnimplemented; callers must treat
  // that as "always invalidate the cache".
  struct Generation {
    uint64_t record_count = 0;
    uint64_t opaque_token = 0;
  };
  virtual absl::StatusOr<Generation> ProbeGeneration(
      absl::string_view tenant_id, absl::string_view session_id) const {
    (void)tenant_id;
    (void)session_id;
    return absl::UnimplementedError(
        "EventSink::ProbeGeneration is not implemented by this backend.");
  }
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_EVENTLOG_EVENT_SINK_H_
