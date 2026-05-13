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

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "absl/functional/function_ref.h"  // from @com_google_absl
#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gtest/gtest.h"
#include "runtime/dpm/event.h"
#include "runtime/platform/eventlog/event_sink.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

std::filesystem::path TestPath(absl::string_view name) {
  std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / std::string(name);
  std::filesystem::remove_all(path);
  return path;
}

TEST(EventSourcedLogTest, AppendsAndReadsEventsInOrder) {
  EventSourcedLog log(TestPath("dpm_event_sourced_log_test"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });

  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .payload = "first",
      .timestamp_us = 100,
  }));
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kCorrection,
      .payload = "second",
      .timestamp_us = 200,
  }));

  ASSERT_OK_AND_ASSIGN(std::vector<Event> events, log.GetAllEvents());
  ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(events[0].type, Event::Type::kUser);
  EXPECT_EQ(events[0].tenant_id, "tenant-a");
  EXPECT_EQ(events[0].session_id, "session-1");
  EXPECT_EQ(events[0].payload, "first");
  EXPECT_EQ(events[0].timestamp_us, 100);
  EXPECT_EQ(events[1].type, Event::Type::kCorrection);
  EXPECT_EQ(events[1].payload, "second");

  ASSERT_OK_AND_ASSIGN(std::vector<Event> events_since,
                       log.GetEventsSince("1"));
  ASSERT_EQ(events_since.size(), 1);
  EXPECT_EQ(events_since[0].payload, "second");
}

TEST(EventSourcedLogTest, RejectsInvalidCheckpoint) {
  EventSourcedLog log(TestPath("dpm_event_sourced_log_checkpoint_test"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  EXPECT_FALSE(log.GetEventsSince("not-an-index").ok());
  EXPECT_FALSE(log.GetEventsSince("-1").ok());
}

TEST(EventSourcedLogTest, RejectsCrossTenantAppend) {
  EventSourcedLog log(TestPath("dpm_event_sourced_log_identity_test"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  EXPECT_FALSE(log.Append(Event{
      .type = Event::Type::kUser,
      .tenant_id = "tenant-b",
      .session_id = "session-1",
      .payload = "wrong tenant",
      .timestamp_us = 100,
  }).ok());
}

TEST(EventSourcedLogTest, DetectsPartialRecord) {
  EventSourcedLog log(TestPath("dpm_event_sourced_log_partial_test"),
                      DPMLogIdentity{
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                      });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .payload = "first",
      .timestamp_us = 100,
  }));
  std::ofstream file(log.path(), std::ios::out | std::ios::app |
                                     std::ios::binary);
  file.put('\1');
  file.close();

  EXPECT_FALSE(log.GetAllEvents().ok());
}

// In-memory EventSink fake exercises the substrate-injected constructor and
// confirms that EventSourcedLog is decoupled from the on-disk format.
class InMemoryEventSink : public EventSink {
 public:
  absl::Status AppendRecord(absl::string_view tenant_id,
                            absl::string_view session_id,
                            absl::string_view record_payload) override {
    auto& bucket = records_[std::string(tenant_id) + "|" +
                            std::string(session_id)];
    bucket.emplace_back(record_payload);
    return absl::OkStatus();
  }
  absl::StatusOr<std::vector<std::string>> ReadRecords(
      absl::string_view tenant_id,
      absl::string_view session_id) const override {
    const std::string key =
        std::string(tenant_id) + "|" + std::string(session_id);
    auto it = records_.find(key);
    if (it == records_.end()) {
      return std::vector<std::string>();
    }
    return it->second;
  }

  void ReplaceRecords(absl::string_view tenant_id, absl::string_view session_id,
                      std::vector<std::string> records) {
    records_[std::string(tenant_id) + "|" + std::string(session_id)] =
        std::move(records);
  }

 private:
  mutable std::map<std::string, std::vector<std::string>> records_;
};

class ProbeEventSink : public EventSink {
 public:
  absl::Status AppendRecord(absl::string_view tenant_id,
                            absl::string_view session_id,
                            absl::string_view record_payload) override {
    records_[std::string(tenant_id) + "|" + std::string(session_id)]
        .emplace_back(record_payload);
    ++generation_.opaque_token;
    return absl::OkStatus();
  }

  absl::StatusOr<std::vector<std::string>> ReadRecords(
      absl::string_view tenant_id,
      absl::string_view session_id) const override {
    const std::string key =
        std::string(tenant_id) + "|" + std::string(session_id);
    auto it = records_.find(key);
    if (it == records_.end()) {
      return std::vector<std::string>();
    }
    return it->second;
  }

  absl::Status ForEachRecord(
      absl::string_view tenant_id, absl::string_view session_id,
      absl::FunctionRef<absl::Status(absl::string_view)> callback)
      const override {
    ++for_each_calls;
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

  absl::Status ForEachRecordRange(
      absl::string_view tenant_id, absl::string_view session_id,
      uint64_t start, uint64_t end,
      absl::FunctionRef<absl::Status(absl::string_view)> callback)
      const override {
    ++for_each_range_calls;
    absl::StatusOr<std::vector<std::string>> records =
        ReadRecords(tenant_id, session_id);
    if (!records.ok()) {
      return records.status();
    }
    for (uint64_t i = start; i < end && i < records->size(); ++i) {
      absl::Status status = callback((*records)[static_cast<size_t>(i)]);
      if (!status.ok()) return status;
    }
    return absl::OkStatus();
  }

  absl::StatusOr<EventSink::Generation> ProbeGeneration(
      absl::string_view tenant_id,
      absl::string_view session_id) const override {
    (void)tenant_id;
    (void)session_id;
    return generation_;
  }

  mutable int for_each_calls = 0;
  mutable int for_each_range_calls = 0;

 private:
  mutable std::map<std::string, std::vector<std::string>> records_;
  EventSink::Generation generation_;
};

TEST(EventSourcedLogTest, RoundTripsThroughInjectedSink) {
  InMemoryEventSink sink;
  EventSourcedLog log(&sink, DPMLogIdentity{
                                 .tenant_id = "tenant-a",
                                 .session_id = "session-1",
                             });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .payload = "first",
      .timestamp_us = 100,
  }));
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kModel,
      .payload = "second",
      .timestamp_us = 200,
  }));

  ASSERT_OK_AND_ASSIGN(std::vector<Event> events, log.GetAllEvents());
  ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(events[0].payload, "first");
  EXPECT_EQ(events[0].tenant_id, "tenant-a");
  EXPECT_EQ(events[1].type, Event::Type::kModel);
  EXPECT_EQ(events[1].timestamp_us, 200);
}

TEST(EventSourcedLogTest, ProbeGenerationReusesEventAndProjectionCaches) {
  ProbeEventSink sink;
  EventSourcedLog log(&sink, DPMLogIdentity{
                                 .tenant_id = "tenant-a",
                                 .session_id = "session-1",
                             });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .payload = "first",
      .timestamp_us = 100,
  }));
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "second",
      .timestamp_us = 200,
  }));

  ASSERT_OK_AND_ASSIGN(std::vector<Event> first_events, log.GetAllEvents());
  ASSERT_EQ(first_events.size(), 2);
  EXPECT_EQ(sink.for_each_calls, 1);

  ASSERT_OK_AND_ASSIGN(std::vector<Event> second_events, log.GetAllEvents());
  ASSERT_EQ(second_events.size(), 2);
  EXPECT_EQ(sink.for_each_calls, 1);

  ASSERT_OK_AND_ASSIGN(std::string first_prompt_log,
                       log.GetProjectionEventLog());
  EXPECT_NE(first_prompt_log.find("[1] {"), std::string::npos);
  EXPECT_EQ(sink.for_each_calls, 2);

  ASSERT_OK_AND_ASSIGN(std::string second_prompt_log,
                       log.GetProjectionEventLog());
  EXPECT_EQ(second_prompt_log, first_prompt_log);
  EXPECT_EQ(sink.for_each_calls, 2);
}

TEST(EventSourcedLogTest, CacheInvalidatesWhenSameRecordCountChanges) {
  InMemoryEventSink sink;
  EventSourcedLog log(&sink, DPMLogIdentity{
                                 .tenant_id = "tenant-a",
                                 .session_id = "session-1",
                             });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .payload = "first",
      .timestamp_us = 100,
  }));
  ASSERT_OK_AND_ASSIGN(std::vector<Event> first_read, log.GetAllEvents());
  ASSERT_EQ(first_read.size(), 1);
  EXPECT_EQ(first_read[0].payload, "first");

  sink.ReplaceRecords("tenant-a", "session-1",
                      {EventToJsonLine(Event{
                          .type = Event::Type::kUser,
                          .tenant_id = "tenant-a",
                          .session_id = "session-1",
                          .payload = "replacement",
                          .timestamp_us = 200,
                      })});

  ASSERT_OK_AND_ASSIGN(std::vector<Event> second_read, log.GetAllEvents());
  ASSERT_EQ(second_read.size(), 1);
  EXPECT_EQ(second_read[0].payload, "replacement");
}

TEST(EventSourcedLogTest, ProjectionEventLogIsPaperIndexedAndCompact) {
  InMemoryEventSink sink;
  EventSourcedLog log(&sink, DPMLogIdentity{
                                 .tenant_id = "tenant-a",
                                 .session_id = "session-1",
                             });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .payload = "first",
      .timestamp_us = 100,
  }));
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "second",
      .timestamp_us = 200,
  }));

  ASSERT_OK_AND_ASSIGN(std::string event_log, log.GetProjectionEventLog());

  EXPECT_NE(event_log.find("[1] {"), std::string::npos);
  EXPECT_NE(event_log.find("[2] {"), std::string::npos);
  EXPECT_EQ(event_log.find("\"index\""), std::string::npos);
  EXPECT_NE(event_log.find("\"payload\":\"first\""), std::string::npos);
  EXPECT_EQ(event_log.find("\"payload\": \"first\""), std::string::npos);
  EXPECT_NE(event_log.find('\n'), std::string::npos);
}

TEST(EventSourcedLogTest, ProjectionRangeUsesRangeIterator) {
  ProbeEventSink sink;
  EventSourcedLog log(&sink, DPMLogIdentity{
                                 .tenant_id = "tenant-a",
                                 .session_id = "session-1",
                             });
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kUser,
      .payload = "first",
      .timestamp_us = 100,
  }));
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kTool,
      .payload = "second",
      .timestamp_us = 200,
  }));
  ASSERT_OK(log.Append(Event{
      .type = Event::Type::kModel,
      .payload = "third",
      .timestamp_us = 300,
  }));

  ASSERT_OK_AND_ASSIGN(std::string event_log,
                       log.GetProjectionEventLogRange(1, 2));
  EXPECT_EQ(sink.for_each_calls, 0);
  EXPECT_EQ(sink.for_each_range_calls, 1);
  EXPECT_EQ(event_log.find("[1] "), std::string::npos);
  EXPECT_NE(event_log.find("[2] "), std::string::npos);
  EXPECT_EQ(event_log.find("[3] "), std::string::npos);
  EXPECT_NE(event_log.find("\"payload\":\"second\""), std::string::npos);
}

}  // namespace
}  // namespace litert::lm
