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

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl

namespace litert::lm {
namespace {

std::string UniqueTempSuffix() {
  static std::atomic<uint64_t> ctr{0};
  const auto thread_id =
      std::hash<std::thread::id>{}(std::this_thread::get_id());
  const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
  return absl::StrCat(".tmp.", thread_id, ".", wall_ns, ".",
                      ctr.fetch_add(1));
}

#ifdef _WIN32

absl::Status WriteAndSyncToTemp(const std::filesystem::path& tmp_path,
                                absl::string_view bytes) {
  HANDLE handle = CreateFileW(
      tmp_path.wstring().c_str(), GENERIC_WRITE,
      /*share=*/0, /*sa=*/nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
      /*template=*/nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return absl::InternalError(absl::StrCat(
        "DurablyWriteFile: CreateFileW failed for ", tmp_path.string()));
  }
  size_t offset = 0;
  while (offset < bytes.size()) {
    const size_t remaining = bytes.size() - offset;
    DWORD chunk = static_cast<DWORD>(remaining < (1u << 20) ? remaining
                                                            : (1u << 20));
    DWORD written = 0;
    if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr)) {
      CloseHandle(handle);
      return absl::InternalError("DurablyWriteFile: WriteFile failed.");
    }
    offset += written;
  }
  if (!FlushFileBuffers(handle)) {
    CloseHandle(handle);
    return absl::InternalError(
        "DurablyWriteFile: FlushFileBuffers failed.");
  }
  CloseHandle(handle);
  return absl::OkStatus();
}

absl::Status ReplaceTempWithTarget(const std::filesystem::path& tmp_path,
                                   const std::filesystem::path& target_path) {
  // MoveFileEx with MOVEFILE_REPLACE_EXISTING is atomic on the same volume.
  if (!MoveFileExW(tmp_path.wstring().c_str(), target_path.wstring().c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return absl::InternalError(
        absl::StrCat("DurablyWriteFile: MoveFileExW failed: ",
                     target_path.string()));
  }
  // No directory fsync on Windows; MOVEFILE_WRITE_THROUGH commits the
  // rename to disk.
  return absl::OkStatus();
}

#else

absl::Status WriteAndSyncToTemp(const std::filesystem::path& tmp_path,
                                absl::string_view bytes) {
  const int fd = open(tmp_path.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                      0640);
  if (fd < 0) {
    return absl::InternalError(absl::StrCat(
        "DurablyWriteFile: open failed for ", tmp_path.string()));
  }
  size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written =
        write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      close(fd);
      return absl::InternalError("DurablyWriteFile: write failed.");
    }
    if (written == 0) {
      close(fd);
      return absl::InternalError("DurablyWriteFile: zero-byte write.");
    }
    offset += static_cast<size_t>(written);
  }
#if defined(__APPLE__)
  if (fsync(fd) != 0) {
#else
  if (fdatasync(fd) != 0) {
#endif
    close(fd);
    return absl::InternalError("DurablyWriteFile: fsync failed.");
  }
  close(fd);
  return absl::OkStatus();
}

absl::Status SyncDirectory(const std::filesystem::path& dir) {
  const int dfd = open(dir.string().c_str(), O_RDONLY);
  if (dfd < 0) {
    return absl::InternalError(absl::StrCat(
        "DurablyWriteFile: open dir failed: ", dir.string()));
  }
  // O_RDONLY directory fsync commits the rename's name entry.
  const int rc = fsync(dfd);
  close(dfd);
  if (rc != 0) {
    return absl::InternalError("DurablyWriteFile: dir fsync failed.");
  }
  return absl::OkStatus();
}

absl::Status ReplaceTempWithTarget(const std::filesystem::path& tmp_path,
                                   const std::filesystem::path& target_path) {
  if (rename(tmp_path.string().c_str(), target_path.string().c_str()) != 0) {
    return absl::InternalError(absl::StrCat(
        "DurablyWriteFile: rename failed: ", target_path.string()));
  }
  return SyncDirectory(target_path.parent_path());
}

#endif  // _WIN32

}  // namespace

absl::Status DurablyWriteFile(const std::filesystem::path& target_path,
                              absl::string_view bytes) {
  std::error_code error;
  std::filesystem::create_directories(target_path.parent_path(), error);
  if (error) {
    return absl::InternalError(absl::StrCat(
        "DurablyWriteFile: create_directories failed: ", error.message()));
  }
  const std::filesystem::path tmp_path =
      target_path.string() + UniqueTempSuffix();
  if (auto status = WriteAndSyncToTemp(tmp_path, bytes); !status.ok()) {
    std::filesystem::remove(tmp_path, error);
    return status;
  }
  if (auto status = ReplaceTempWithTarget(tmp_path, target_path);
      !status.ok()) {
    std::filesystem::remove(tmp_path, error);
    return status;
  }
  return absl::OkStatus();
}

absl::Status DurablyCreateNewFile(const std::filesystem::path& target_path,
                                  absl::string_view bytes) {
  std::error_code error;
  std::filesystem::create_directories(target_path.parent_path(), error);
  if (error) {
    return absl::InternalError(absl::StrCat(
        "DurablyCreateNewFile: create_directories failed: ",
        error.message()));
  }
  const std::filesystem::path tmp_path =
      target_path.string() + UniqueTempSuffix();
  if (auto status = WriteAndSyncToTemp(tmp_path, bytes); !status.ok()) {
    std::filesystem::remove(tmp_path, error);
    return status;
  }

#ifdef _WIN32
  if (!MoveFileExW(tmp_path.wstring().c_str(), target_path.wstring().c_str(),
                   MOVEFILE_WRITE_THROUGH)) {
    const DWORD err = GetLastError();
    std::filesystem::remove(tmp_path, error);
    if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) {
      return absl::AlreadyExistsError(absl::StrCat(
          "DurablyCreateNewFile: target already exists: ",
          target_path.string()));
    }
    return absl::InternalError(absl::StrCat(
        "DurablyCreateNewFile: MoveFileExW failed (err=", err, ") for ",
        target_path.string()));
  }
  return absl::OkStatus();
#else
  if (link(tmp_path.string().c_str(), target_path.string().c_str()) != 0) {
    const int err = errno;
    std::filesystem::remove(tmp_path, error);
    if (err == EEXIST) {
      return absl::AlreadyExistsError(absl::StrCat(
          "DurablyCreateNewFile: target already exists: ",
          target_path.string()));
    }
    return absl::InternalError(absl::StrCat(
        "DurablyCreateNewFile: link failed for ", target_path.string(),
        " (errno=", err, ")."));
  }
  if (auto status = SyncDirectory(target_path.parent_path()); !status.ok()) {
    std::filesystem::remove(tmp_path, error);
    return status;
  }
  std::filesystem::remove(tmp_path, error);
  if (error) {
    return absl::InternalError(absl::StrCat(
        "DurablyCreateNewFile: failed to remove temp file: ",
        error.message()));
  }
  return SyncDirectory(target_path.parent_path());
#endif
}

absl::Status ReadEntireFileIfExists(const std::filesystem::path& path,
                                    std::string* out) {
  if (!std::filesystem::exists(path)) {
    out->clear();
    return absl::OkStatus();
  }
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) {
    return absl::InternalError(absl::StrCat(
        "ReadEntireFileIfExists: open failed: ", path.string()));
  }
  std::stringstream buf;
  buf << in.rdbuf();
  *out = buf.str();
  return absl::OkStatus();
}

}  // namespace litert::lm
