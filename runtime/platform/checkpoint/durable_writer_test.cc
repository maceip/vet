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

#include "runtime/platform/checkpoint/durable_writer.h"

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gtest/gtest.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

std::filesystem::path TestRoot(absl::string_view name) {
  std::filesystem::path path =
      std::filesystem::path(::testing::TempDir()) / std::string(name);
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  return path;
}

TEST(DurableWriterTest, RoundTrip) {
  std::filesystem::path root = TestRoot("durable_writer_roundtrip");
  std::filesystem::path target = root / "subdir" / "file.bin";
  ASSERT_OK(DurablyWriteFile(target, "hello-durable"));
  std::string out;
  ASSERT_OK(ReadEntireFileIfExists(target, &out));
  EXPECT_EQ(out, "hello-durable");
}

TEST(DurableWriterTest, OverwriteIsAtomic) {
  std::filesystem::path root = TestRoot("durable_writer_overwrite");
  std::filesystem::path target = root / "file.bin";
  ASSERT_OK(DurablyWriteFile(target, "first"));
  ASSERT_OK(DurablyWriteFile(target, "second"));
  std::string out;
  ASSERT_OK(ReadEntireFileIfExists(target, &out));
  EXPECT_EQ(out, "second");
}

TEST(DurableWriterTest, NoStrayTempFilesOnSuccess) {
  std::filesystem::path root = TestRoot("durable_writer_no_temp");
  std::filesystem::path target = root / "file.bin";
  ASSERT_OK(DurablyWriteFile(target, "x"));
  // The directory should contain exactly one entry (the target). Temp
  // files are removed by the rename step.
  int count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    (void)entry;
    ++count;
  }
  EXPECT_EQ(count, 1);
}

TEST(DurableWriterTest, ConcurrentWritersToDifferentTargetsDoNotCollide) {
  std::filesystem::path root = TestRoot("durable_writer_concurrent");
  std::vector<std::thread> threads;
  for (int i = 0; i < 16; ++i) {
    threads.emplace_back([root, i] {
      std::filesystem::path target = root / absl::StrCat("file-", i, ".bin");
      ASSERT_OK(DurablyWriteFile(target, absl::StrCat("payload-", i)));
    });
  }
  for (auto& t : threads) t.join();
  for (int i = 0; i < 16; ++i) {
    std::string out;
    ASSERT_OK(ReadEntireFileIfExists(root / absl::StrCat("file-", i, ".bin"),
                                     &out));
    EXPECT_EQ(out, absl::StrCat("payload-", i));
  }
}

TEST(DurableWriterTest, CreateNewFileRejectsExistingTarget) {
  std::filesystem::path root = TestRoot("durable_writer_create_new_existing");
  std::filesystem::path target = root / "file.bin";
  ASSERT_OK(DurablyCreateNewFile(target, "first"));
  EXPECT_EQ(DurablyCreateNewFile(target, "second").code(),
            absl::StatusCode::kAlreadyExists);
  std::string out;
  ASSERT_OK(ReadEntireFileIfExists(target, &out));
  EXPECT_EQ(out, "first");
}

TEST(DurableWriterTest, ConcurrentCreateNewFilePublishesOnce) {
  std::filesystem::path root = TestRoot("durable_writer_create_new_race");
  std::filesystem::path target = root / "file.bin";
  std::vector<std::thread> threads;
  std::vector<absl::Status> statuses(16);
  for (int i = 0; i < 16; ++i) {
    threads.emplace_back([target, &statuses, i] {
      statuses[i] = DurablyCreateNewFile(target, absl::StrCat("payload-", i));
    });
  }
  for (auto& t : threads) t.join();

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
  EXPECT_EQ(already_exists_count, 15);
}

TEST(ReadEntireFileIfExistsTest, MissingFileReturnsEmpty) {
  std::filesystem::path root = TestRoot("read_if_exists_missing");
  std::string out = "stale";
  ASSERT_OK(ReadEntireFileIfExists(root / "nope", &out));
  EXPECT_EQ(out, "");
}

}  // namespace
}  // namespace litert::lm
