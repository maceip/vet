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

#include "runtime/platform/eventlog/posix_event_sink.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gtest/gtest.h"
#include "runtime/platform/eventlog/event_sink.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

std::filesystem::path TestRoot(absl::string_view name) {
  std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / std::string(name);
  std::filesystem::remove_all(path);
  return path;
}

TEST(PosixEventSinkTest, AppendsAndReadsBytesInOrder) {
  PosixEventSink sink(TestRoot("posix_event_sink_basic"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-1", "first"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-1", "second"));

  ASSERT_OK_AND_ASSIGN(std::vector<std::string> records,
                       sink.ReadRecords("tenant-a", "session-1"));
  ASSERT_EQ(records.size(), 2);
  EXPECT_EQ(records[0], "first");
  EXPECT_EQ(records[1], "second");
}

TEST(PosixEventSinkTest, IsolatesTenantsAndSessionsByPath) {
  PosixEventSink sink(TestRoot("posix_event_sink_isolation"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-1", "AAA"));
  ASSERT_OK(sink.AppendRecord("tenant-b", "session-1", "BBB"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-2", "CCC"));

  ASSERT_OK_AND_ASSIGN(auto a1, sink.ReadRecords("tenant-a", "session-1"));
  ASSERT_OK_AND_ASSIGN(auto b1, sink.ReadRecords("tenant-b", "session-1"));
  ASSERT_OK_AND_ASSIGN(auto a2, sink.ReadRecords("tenant-a", "session-2"));
  ASSERT_EQ(a1.size(), 1);
  EXPECT_EQ(a1[0], "AAA");
  ASSERT_EQ(b1.size(), 1);
  EXPECT_EQ(b1[0], "BBB");
  ASSERT_EQ(a2.size(), 1);
  EXPECT_EQ(a2[0], "CCC");
}

TEST(PosixEventSinkTest, RejectsPathTraversalIdentities) {
  PosixEventSink sink(TestRoot("posix_event_sink_traversal"));
  EXPECT_FALSE(sink.AppendRecord("..", "session-1", "x").ok());
  EXPECT_FALSE(sink.AppendRecord("tenant-a", "..", "x").ok());
  EXPECT_FALSE(sink.AppendRecord("ten/ant", "session-1", "x").ok());
  EXPECT_FALSE(sink.AppendRecord("", "session-1", "x").ok());
}

TEST(PosixEventSinkTest, RejectsJsonUnsafeIdentities) {
  PosixEventSink sink(TestRoot("posix_event_sink_json_unsafe"));
  EXPECT_FALSE(sink.AppendRecord("tenant\"a", "session-1", "x").ok());
  EXPECT_FALSE(sink.AppendRecord("tenant\\a", "session-1", "x").ok());
  EXPECT_FALSE(sink.AppendRecord("tenant\na", "session-1", "x").ok());
  EXPECT_FALSE(sink.AppendRecord("tenant:a", "session-1", "x").ok());

  ASSERT_OK(sink.AppendRecord("tenant.v2", "session.1", "x"));
  ASSERT_OK_AND_ASSIGN(std::vector<std::string> records,
                       sink.ReadRecords("tenant.v2", "session.1"));
  ASSERT_EQ(records.size(), 1);
  EXPECT_EQ(records[0], "x");
}

TEST(PosixEventSinkTest, RejectsEmptyPayloadToProtectReaderInvariant) {
  PosixEventSink sink(TestRoot("posix_event_sink_empty_payload"));
  EXPECT_FALSE(sink.AppendRecord("tenant-a", "session-1", "").ok());
  ASSERT_OK_AND_ASSIGN(std::vector<std::string> records,
                       sink.ReadRecords("tenant-a", "session-1"));
  EXPECT_TRUE(records.empty());
}

TEST(PosixEventSinkTest, ReturnsEmptyForUnknownSession) {
  PosixEventSink sink(TestRoot("posix_event_sink_empty"));
  ASSERT_OK_AND_ASSIGN(std::vector<std::string> records,
                       sink.ReadRecords("tenant-a", "session-1"));
  EXPECT_TRUE(records.empty());
}

TEST(PosixEventSinkTest, SerializesConcurrentSameProcessAppends) {
  PosixEventSink sink(TestRoot("posix_event_sink_concurrent"));
  constexpr int kPerWriter = 200;
  auto writer = [&sink](absl::string_view tag,
                        std::vector<absl::Status>* statuses) {
    statuses->reserve(kPerWriter);
    for (int i = 0; i < kPerWriter; ++i) {
      statuses->push_back(sink.AppendRecord(
          "tenant-a", "session-1", std::string(tag) + "-" + std::to_string(i)));
    }
  };
  std::vector<absl::Status> statuses1;
  std::vector<absl::Status> statuses2;
  std::thread t1(writer, "AAA", &statuses1);
  std::thread t2(writer, "BBB", &statuses2);
  t1.join();
  t2.join();
  for (const absl::Status& status : statuses1) {
    ASSERT_OK(status);
  }
  for (const absl::Status& status : statuses2) {
    ASSERT_OK(status);
  }

  ASSERT_OK_AND_ASSIGN(std::vector<std::string> records,
                       sink.ReadRecords("tenant-a", "session-1"));
  ASSERT_EQ(records.size(), 2 * kPerWriter);
  int aaa = 0, bbb = 0;
  for (const std::string& r : records) {
    if (r.rfind("AAA-", 0) == 0) {
      ++aaa;
    } else if (r.rfind("BBB-", 0) == 0) {
      ++bbb;
    }
  }
  EXPECT_EQ(aaa, kPerWriter);
  EXPECT_EQ(bbb, kPerWriter);
}

TEST(PosixEventSinkTest, ProbeGenerationAdvancesOnAppend) {
  PosixEventSink sink(TestRoot("posix_event_sink_probe"));
  ASSERT_OK_AND_ASSIGN(EventSink::Generation g0,
                       sink.ProbeGeneration("tenant-a", "session-1"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-1", "first"));
  ASSERT_OK_AND_ASSIGN(EventSink::Generation g1,
                       sink.ProbeGeneration("tenant-a", "session-1"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-1", "second"));
  ASSERT_OK_AND_ASSIGN(EventSink::Generation g2,
                       sink.ProbeGeneration("tenant-a", "session-1"));
  EXPECT_LT(g0.opaque_token, g1.opaque_token);
  EXPECT_LT(g1.opaque_token, g2.opaque_token);
}

TEST(PosixEventSinkTest, WritesRetentionSidecarWhenPolicyProvided) {
  PosixEventSink sink(TestRoot("posix_event_sink_retention"));
  EventSink::RetentionPolicy policy;
  policy.retain_until_unix_seconds = 1777089600;
  policy.legal_hold = true;
  ASSERT_OK(sink.AppendRecordWithRetention("tenant-a", "session-1",
                                           "first", policy));

  const std::filesystem::path sidecar =
      sink.RetentionSidecarPathFor("tenant-a", "session-1");
  ASSERT_TRUE(std::filesystem::exists(sidecar));
  std::ifstream in(sidecar);
  std::string contents((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  EXPECT_NE(contents.find("\"retain_until_unix_seconds\":1777089600"),
            std::string::npos);
  EXPECT_NE(contents.find("\"legal_hold\":true"), std::string::npos);

  // Empty policy must not create a sidecar.
  PosixEventSink sink_no_policy(TestRoot("posix_event_sink_no_retention"));
  ASSERT_OK(sink_no_policy.AppendRecord("tenant-a", "session-1", "x"));
  EXPECT_FALSE(std::filesystem::exists(
      sink_no_policy.RetentionSidecarPathFor("tenant-a", "session-1")));
}

TEST(PosixEventSinkTest, RetentionSidecarOverwritesOnEachCall) {
  PosixEventSink sink(TestRoot("posix_event_sink_retention_rewrite"));
  EventSink::RetentionPolicy first;
  first.retain_until_unix_seconds = 100;
  ASSERT_OK(sink.AppendRecordWithRetention("tenant-a", "session-1",
                                           "first", first));
  EventSink::RetentionPolicy second;
  second.retain_until_unix_seconds = 200;
  second.legal_hold = true;
  ASSERT_OK(sink.AppendRecordWithRetention("tenant-a", "session-1",
                                           "second", second));

  std::ifstream in(sink.RetentionSidecarPathFor("tenant-a", "session-1"));
  std::string contents((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  EXPECT_NE(contents.find("200"), std::string::npos);
  EXPECT_EQ(contents.find("100"), std::string::npos);
  EXPECT_NE(contents.find("true"), std::string::npos);
}

TEST(PosixEventSinkTest, BranchSeesParentRecordsThenItsOwn) {
  PosixEventSink sink(TestRoot("posix_event_sink_branch_basic"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-1", "p0"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-1", "p1"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-1", "p2"));

  ASSERT_OK(sink.CreateBranch("tenant-a", "session-1",
                              "tenant-a", "branch-X",
                              /*parent_record_count_at_branch=*/2));

  // Parent appends after branch should not show up on the branch.
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-1", "p3"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "branch-X", "b0"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "branch-X", "b1"));

  ASSERT_OK_AND_ASSIGN(std::vector<std::string> branch_records,
                       sink.ReadRecords("tenant-a", "branch-X"));
  ASSERT_EQ(branch_records.size(), 4);
  EXPECT_EQ(branch_records[0], "p0");
  EXPECT_EQ(branch_records[1], "p1");
  EXPECT_EQ(branch_records[2], "b0");
  EXPECT_EQ(branch_records[3], "b1");

  // Parent's view is unchanged by the branch's existence.
  ASSERT_OK_AND_ASSIGN(std::vector<std::string> parent_records,
                       sink.ReadRecords("tenant-a", "session-1"));
  ASSERT_EQ(parent_records.size(), 4);
  EXPECT_EQ(parent_records[3], "p3");
}

TEST(PosixEventSinkTest, BranchAtZeroRecordsYieldsEmptyParentSlice) {
  PosixEventSink sink(TestRoot("posix_event_sink_branch_zero"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-1", "p0"));
  ASSERT_OK(sink.CreateBranch("tenant-a", "session-1",
                              "tenant-a", "branch-Y", 0));
  ASSERT_OK(sink.AppendRecord("tenant-a", "branch-Y", "b0"));

  ASSERT_OK_AND_ASSIGN(std::vector<std::string> branch_records,
                       sink.ReadRecords("tenant-a", "branch-Y"));
  ASSERT_EQ(branch_records.size(), 1);
  EXPECT_EQ(branch_records[0], "b0");
}

TEST(PosixEventSinkTest, BranchOfBranchSeesGrandparentSlice) {
  PosixEventSink sink(TestRoot("posix_event_sink_branch_of_branch"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "root", "g0"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "root", "g1"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "root", "g2"));
  ASSERT_OK(sink.CreateBranch("tenant-a", "root", "tenant-a", "B", 2));
  ASSERT_OK(sink.AppendRecord("tenant-a", "B", "b0"));
  ASSERT_OK(sink.CreateBranch("tenant-a", "B", "tenant-a", "C", 3));
  ASSERT_OK(sink.AppendRecord("tenant-a", "C", "c0"));

  ASSERT_OK_AND_ASSIGN(std::vector<std::string> records,
                       sink.ReadRecords("tenant-a", "C"));
  ASSERT_EQ(records.size(), 4);
  EXPECT_EQ(records[0], "g0");
  EXPECT_EQ(records[1], "g1");
  EXPECT_EQ(records[2], "b0");
  EXPECT_EQ(records[3], "c0");
}

TEST(PosixEventSinkTest, CreateBranchRejectsOversizedOffset) {
  PosixEventSink sink(TestRoot("posix_event_sink_branch_oversize"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-1", "p0"));
  EXPECT_FALSE(sink.CreateBranch("tenant-a", "session-1",
                                 "tenant-a", "branch", 99).ok());
}

TEST(PosixEventSinkTest, CreateBranchRejectsCollidingIdentity) {
  PosixEventSink sink(TestRoot("posix_event_sink_branch_collide"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-1", "p0"));
  ASSERT_OK(sink.CreateBranch("tenant-a", "session-1",
                              "tenant-a", "branch", 1));
  EXPECT_FALSE(sink.CreateBranch("tenant-a", "session-1",
                                 "tenant-a", "branch", 1).ok());
}

TEST(PosixEventSinkTest, ConcurrentCreateBranchPublishesOnce) {
  PosixEventSink sink(TestRoot("posix_event_sink_branch_concurrent"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-1", "p0"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-1", "p1"));

  constexpr int kWriters = 16;
  std::vector<absl::Status> statuses(kWriters);
  std::vector<std::thread> threads;
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  threads.reserve(kWriters);
  for (int i = 0; i < kWriters; ++i) {
    threads.emplace_back([&sink, &statuses, &ready, &start, i]() {
      ready.fetch_add(1);
      while (!start.load()) {
        std::this_thread::yield();
      }
      statuses[i] = sink.CreateBranch("tenant-a", "session-1",
                                      "tenant-a", "branch-race", 2);
    });
  }
  while (ready.load() != kWriters) {
    std::this_thread::yield();
  }
  start.store(true);
  for (std::thread& thread : threads) {
    thread.join();
  }

  int ok_count = 0;
  int already_exists_count = 0;
  for (const absl::Status& status : statuses) {
    if (status.ok()) {
      ++ok_count;
    } else if (status.code() == absl::StatusCode::kAlreadyExists) {
      ++already_exists_count;
    }
  }
  EXPECT_EQ(ok_count, 1);
  EXPECT_EQ(already_exists_count, kWriters - 1);

  ASSERT_OK_AND_ASSIGN(std::vector<std::string> branch_records,
                       sink.ReadRecords("tenant-a", "branch-race"));
  ASSERT_EQ(branch_records.size(), 2);
  EXPECT_EQ(branch_records[0], "p0");
  EXPECT_EQ(branch_records[1], "p1");
}

TEST(PosixEventSinkTest, CreateBranchRejectsSelfBranch) {
  PosixEventSink sink(TestRoot("posix_event_sink_branch_self"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-1", "p0"));
  EXPECT_FALSE(sink.CreateBranch("tenant-a", "session-1",
                                 "tenant-a", "session-1", 1).ok());
}

TEST(PosixEventSinkTest, DetectsCorruptedTrailingByte) {
  PosixEventSink sink(TestRoot("posix_event_sink_partial"));
  ASSERT_OK(sink.AppendRecord("tenant-a", "session-1", "first"));
  std::ofstream file(sink.PathFor("tenant-a", "session-1"),
                     std::ios::out | std::ios::app | std::ios::binary);
  file.put('\1');
  file.close();
  EXPECT_FALSE(sink.ReadRecords("tenant-a", "session-1").ok());
}

}  // namespace
}  // namespace litert::lm
