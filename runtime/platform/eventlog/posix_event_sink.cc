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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "absl/functional/function_ref.h"  // from @com_google_absl
#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "absl/types/optional.h"  // from @com_google_absl
#include "runtime/platform/checkpoint/durable_writer.h"
#include "runtime/util/memory_mapped_file.h"
#include "runtime/util/status_macros.h"

namespace litert::lm {
namespace {

constexpr std::array<char, 8> kLogMagic = {'D', 'P', 'M', 'L',
                                           'O', 'G', '1', '\n'};
constexpr uint32_t kMaxRecordBytes = 64 * 1024 * 1024;
constexpr int kMaxBranchDepth = 16;

// Parsed contents of branch_pointer.json.
struct BranchPointer {
  std::string parent_tenant_id;
  std::string parent_session_id;
  uint64_t parent_record_count_at_branch = 0;
};

// JSON string escaper used when writing branch_pointer.json. Identity
// validation rejects characters that would require escaping in the current
// parser, but keeping the writer escaped makes the sidecar robust if that
// validator is loosened later.
void AppendJsonEscapedString(absl::string_view s, std::string* out) {
  constexpr char kHex[] = "0123456789abcdef";
  for (char c : s) {
    const auto u = static_cast<unsigned char>(c);
    switch (c) {
      case '\\':
        out->append("\\\\");
        break;
      case '"':
        out->append("\\\"");
        break;
      case '\b':
        out->append("\\b");
        break;
      case '\f':
        out->append("\\f");
        break;
      case '\n':
        out->append("\\n");
        break;
      case '\r':
        out->append("\\r");
        break;
      case '\t':
        out->append("\\t");
        break;
      default:
        if (u < 0x20) {
          // \u00XX for any other control character.
          out->append("\\u00");
          out->push_back(kHex[(u >> 4) & 0x0f]);
          out->push_back(kHex[u & 0x0f]);
        } else {
          out->push_back(c);
        }
    }
  }
}

std::string SerializeBranchPointer(const BranchPointer& bp) {
  std::string out;
  out.reserve(128 + bp.parent_tenant_id.size() + bp.parent_session_id.size());
  out.append("{\"version\":1,\"parent_tenant_id\":\"");
  AppendJsonEscapedString(bp.parent_tenant_id, &out);
  out.append("\",\"parent_session_id\":\"");
  AppendJsonEscapedString(bp.parent_session_id, &out);
  out.append("\",\"parent_record_count_at_branch\":");
  out.append(std::to_string(bp.parent_record_count_at_branch));
  out.append("}\n");
  return out;
}

absl::StatusOr<std::string> ExtractJsonString(absl::string_view& view,
                                              absl::string_view key) {
  const std::string needle = absl::StrCat("\"", key, "\":\"");
  const size_t pos = view.find(needle);
  if (pos == absl::string_view::npos) {
    return absl::DataLossError(
        absl::StrCat("branch_pointer.json missing key ", key));
  }
  view.remove_prefix(pos + needle.size());
  const size_t end = view.find('"');
  if (end == absl::string_view::npos) {
    return absl::DataLossError("branch_pointer.json: unterminated string.");
  }
  std::string out(view.data(), end);
  view.remove_prefix(end + 1);
  return out;
}

absl::StatusOr<uint64_t> ExtractJsonUint(absl::string_view& view,
                                         absl::string_view key) {
  const std::string needle = absl::StrCat("\"", key, "\":");
  const size_t pos = view.find(needle);
  if (pos == absl::string_view::npos) {
    return absl::DataLossError(
        absl::StrCat("branch_pointer.json missing key ", key));
  }
  view.remove_prefix(pos + needle.size());
  uint64_t value = 0;
  size_t consumed = 0;
  while (consumed < view.size() && view[consumed] >= '0' &&
         view[consumed] <= '9') {
    value = value * 10 + (view[consumed] - '0');
    ++consumed;
  }
  if (consumed == 0) {
    return absl::DataLossError(
        absl::StrCat("branch_pointer.json key ", key, " is not numeric."));
  }
  view.remove_prefix(consumed);
  return value;
}

absl::StatusOr<BranchPointer> ParseBranchPointer(absl::string_view bytes) {
  BranchPointer bp;
  ASSIGN_OR_RETURN(bp.parent_tenant_id,
                   ExtractJsonString(bytes, "parent_tenant_id"));
  ASSIGN_OR_RETURN(bp.parent_session_id,
                   ExtractJsonString(bytes, "parent_session_id"));
  ASSIGN_OR_RETURN(bp.parent_record_count_at_branch,
                   ExtractJsonUint(bytes, "parent_record_count_at_branch"));
  return bp;
}

// Process-local serialization for access to a given path. POSIX fcntl/F_SETLKW
// locks are per-process: two threads in the same process can each hold the
// "exclusive" file lock simultaneously, and same-process readers can observe a
// write in progress. The path-keyed mutex below closes that gap. Cross-process
// serialization still relies on the file lock taken inside LockedFile::Open.
class PathMutexRegistry {
 public:
  static PathMutexRegistry& Instance() {
    static auto* registry = new PathMutexRegistry();
    return *registry;
  }
  std::shared_ptr<std::mutex> Acquire(const std::string& key) {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    auto& slot = mutexes_[key];
    if (!slot) {
      slot = std::make_shared<std::mutex>();
    }
    return slot;
  }

 private:
  std::mutex registry_mutex_;
  std::unordered_map<std::string, std::shared_ptr<std::mutex>> mutexes_;
};

// Identity components are path components and branch_pointer.json string
// fields. Reject:
//   - empty / "." / ".."
//   - path separators and filesystem meta characters
//   - JSON string terminators / escape bytes
//   - control bytes
bool IsValidIdentityComponent(absl::string_view value) {
  if (value.empty() || value == "." || value == "..") return false;
  for (char c : value) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || c == '/' || c == '\\' || c == '"' || c == '<' ||
        c == '>' || c == ':' || c == '|' || c == '?' || c == '*') {
      return false;
    }
  }
  return true;
}

uint32_t ReadLittleEndian32(const char* data) {
  return static_cast<uint32_t>(static_cast<unsigned char>(data[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(data[3])) << 24);
}

void AppendLittleEndian32(uint32_t value, std::string* out) {
  out->push_back(static_cast<char>(value & 0xff));
  out->push_back(static_cast<char>((value >> 8) & 0xff));
  out->push_back(static_cast<char>((value >> 16) & 0xff));
  out->push_back(static_cast<char>((value >> 24) & 0xff));
}

std::string MakeRecord(absl::string_view payload) {
  std::string record;
  record.reserve(sizeof(uint32_t) * 2 + payload.size());
  const uint32_t size = static_cast<uint32_t>(payload.size());
  AppendLittleEndian32(size, &record);
  AppendLittleEndian32(~size, &record);
  record.append(payload.data(), payload.size());
  return record;
}

absl::Status ValidateIdentity(absl::string_view tenant_id,
                              absl::string_view session_id) {
  if (!IsValidIdentityComponent(tenant_id)) {
    return absl::InvalidArgumentError(
        "tenant_id must be non-empty and must not contain reserved path or "
        "JSON characters.");
  }
  if (!IsValidIdentityComponent(session_id)) {
    return absl::InvalidArgumentError(
        "session_id must be non-empty and must not contain reserved path or "
        "JSON characters.");
  }
  return absl::OkStatus();
}

#ifdef _WIN32

class LockedFile {
 public:
  explicit LockedFile(HANDLE handle) : handle_(handle) {}
  LockedFile(const LockedFile&) = delete;
  LockedFile& operator=(const LockedFile&) = delete;
  LockedFile(LockedFile&& other) noexcept
      : handle_(other.handle_), lock_overlapped_(other.lock_overlapped_) {
    other.handle_ = INVALID_HANDLE_VALUE;
    other.lock_overlapped_ = {};
  }
  ~LockedFile() {
    if (handle_ == INVALID_HANDLE_VALUE) {
      return;
    }
    UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &lock_overlapped_);
    CloseHandle(handle_);
  }

  static absl::StatusOr<LockedFile> Open(const std::filesystem::path& path,
                                         bool exclusive, bool create) {
    const DWORD access = exclusive ? (GENERIC_READ | GENERIC_WRITE)
                                   : GENERIC_READ;
    const DWORD disposition = create ? OPEN_ALWAYS : OPEN_EXISTING;
    HANDLE handle =
        CreateFileW(path.wstring().c_str(), access,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, disposition,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      return absl::InternalError(
          absl::StrCat("Failed to open DPM log file: ", path.string()));
    }

    OVERLAPPED overlapped = {};
    DWORD flags = exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if (!LockFileEx(handle, flags, 0, MAXDWORD, MAXDWORD, &overlapped)) {
      CloseHandle(handle);
      return absl::InternalError(
          absl::StrCat("Failed to lock DPM log file: ", path.string()));
    }
    LockedFile file(handle);
    file.lock_overlapped_ = overlapped;
    return file;
  }

  absl::StatusOr<uint64_t> Size() const {
    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle_, &size)) {
      return absl::InternalError("Failed to read DPM log file size.");
    }
    return static_cast<uint64_t>(size.QuadPart);
  }

  absl::Status Append(absl::string_view bytes) {
    LARGE_INTEGER zero = {};
    if (!SetFilePointerEx(handle_, zero, nullptr, FILE_END)) {
      return absl::InternalError("Failed to seek DPM log file for append.");
    }
    size_t offset = 0;
    while (offset < bytes.size()) {
      DWORD bytes_written = 0;
      const size_t remaining = bytes.size() - offset;
      const DWORD chunk_size =
          static_cast<DWORD>(remaining < (1 << 20) ? remaining : (1 << 20));
      if (!WriteFile(handle_, bytes.data() + offset, chunk_size,
                     &bytes_written, nullptr)) {
        return absl::InternalError("Failed to write DPM log file.");
      }
      if (bytes_written == 0) {
        return absl::InternalError("Failed to make progress writing DPM log.");
      }
      offset += bytes_written;
    }
    if (!FlushFileBuffers(handle_)) {
      return absl::InternalError("Failed to fsync DPM log file.");
    }
    return absl::OkStatus();
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  OVERLAPPED lock_overlapped_ = {};
};

#else

class LockedFile {
 public:
  explicit LockedFile(int fd) : fd_(fd) {}
  LockedFile(const LockedFile&) = delete;
  LockedFile& operator=(const LockedFile&) = delete;
  LockedFile(LockedFile&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }
  ~LockedFile() {
    if (fd_ < 0) {
      return;
    }
    struct flock lock = {};
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    fcntl(fd_, F_SETLK, &lock);
    close(fd_);
  }

  static absl::StatusOr<LockedFile> Open(const std::filesystem::path& path,
                                         bool exclusive, bool create) {
    const int flags = exclusive ? ((create ? O_CREAT : 0) | O_APPEND | O_RDWR)
                                : O_RDONLY;
    const int fd = open(path.string().c_str(), flags, 0640);
    if (fd < 0) {
      return absl::InternalError(
          absl::StrCat("Failed to open DPM log file: ", path.string()));
    }

    struct flock lock = {};
    lock.l_type = exclusive ? F_WRLCK : F_RDLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLKW, &lock) != 0) {
      close(fd);
      return absl::InternalError(
          absl::StrCat("Failed to lock DPM log file: ", path.string()));
    }
    return LockedFile(fd);
  }

  absl::StatusOr<uint64_t> Size() const {
    struct stat stat_buffer = {};
    if (fstat(fd_, &stat_buffer) != 0) {
      return absl::InternalError("Failed to read DPM log file size.");
    }
    return static_cast<uint64_t>(stat_buffer.st_size);
  }

  absl::Status Append(absl::string_view bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
      const ssize_t written =
          write(fd_, bytes.data() + offset, bytes.size() - offset);
      if (written <= 0) {
        return absl::InternalError("Failed to write DPM log file.");
      }
      offset += static_cast<size_t>(written);
    }
#if defined(__APPLE__)
    if (fsync(fd_) != 0) {
#else
    if (fdatasync(fd_) != 0) {
#endif
      return absl::InternalError("Failed to fsync DPM log file.");
    }
    return absl::OkStatus();
  }

 private:
  int fd_ = -1;
};

#endif

absl::Status ForEachMappedRecord(
    MemoryMappedFile& mapped_file,
    absl::FunctionRef<absl::Status(absl::string_view)> callback) {
  if (mapped_file.length() == 0) {
    return absl::OkStatus();
  }
  if (mapped_file.length() < kLogMagic.size()) {
    return absl::DataLossError("DPM event log has a partial header.");
  }
  const char* data = static_cast<const char*>(mapped_file.data());
  if (std::memcmp(data, kLogMagic.data(), kLogMagic.size()) != 0) {
    return absl::DataLossError("DPM event log has an invalid magic header.");
  }

  size_t offset = kLogMagic.size();
  while (offset < mapped_file.length()) {
    if (mapped_file.length() - offset < sizeof(uint32_t) * 2) {
      return absl::DataLossError(
          "DPM event log has a partial length-prefixed record.");
    }
    const uint32_t record_size = ReadLittleEndian32(data + offset);
    const uint32_t complement =
        ReadLittleEndian32(data + offset + sizeof(uint32_t));
    offset += sizeof(uint32_t) * 2;
    if (record_size == 0 || record_size > kMaxRecordBytes ||
        complement != ~record_size) {
      return absl::DataLossError("DPM event log record length is corrupt.");
    }
    if (mapped_file.length() - offset < record_size) {
      return absl::DataLossError("DPM event log has a partial record body.");
    }
    RETURN_IF_ERROR(callback(absl::string_view(data + offset, record_size)));
    offset += record_size;
  }
  return absl::OkStatus();
}

}  // namespace

PosixEventSink::PosixEventSink(std::filesystem::path root_path)
    : root_path_(std::move(root_path)) {}

std::filesystem::path PosixEventSink::PathFor(
    absl::string_view tenant_id, absl::string_view session_id) const {
  return root_path_ / std::string(tenant_id) / std::string(session_id) /
         "events.dpmlog";
}

std::filesystem::path PosixEventSink::RetentionSidecarPathFor(
    absl::string_view tenant_id, absl::string_view session_id) const {
  return root_path_ / std::string(tenant_id) / std::string(session_id) /
         "events.dpmlog.retention.json";
}

std::filesystem::path PosixEventSink::BranchPointerPathFor(
    absl::string_view tenant_id, absl::string_view session_id) const {
  return root_path_ / std::string(tenant_id) / std::string(session_id) /
         "branch_pointer.json";
}

absl::Status PosixEventSink::AppendRecord(absl::string_view tenant_id,
                                          absl::string_view session_id,
                                          absl::string_view record_payload) {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  if (record_payload.empty()) {
    return absl::InvalidArgumentError(
        "DPM event payload must not be empty; the on-disk format reserves "
        "size==0 as a corruption sentinel.");
  }
  if (record_payload.size() > kMaxRecordBytes) {
    return absl::InvalidArgumentError("DPM event payload is too large.");
  }

  const std::filesystem::path path = PathFor(tenant_id, session_id);
  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
      return absl::InternalError(absl::StrCat(
          "Failed to create DPM event log directory '", parent.string(),
          "': ", error.message()));
    }
  }

  std::shared_ptr<std::mutex> path_mutex =
      PathMutexRegistry::Instance().Acquire(path.string());
  std::lock_guard<std::mutex> guard(*path_mutex);

  ASSIGN_OR_RETURN(LockedFile file,
                   LockedFile::Open(path, /*exclusive=*/true,
                                    /*create=*/true));
  ASSIGN_OR_RETURN(uint64_t file_size, file.Size());
  std::string bytes;
  if (file_size == 0) {
    bytes.append(kLogMagic.data(), kLogMagic.size());
  }
  bytes.append(MakeRecord(record_payload));
  return file.Append(bytes);
}

absl::Status PosixEventSink::AppendRecordWithRetention(
    absl::string_view tenant_id, absl::string_view session_id,
    absl::string_view record_payload, const RetentionPolicy& retention) {
  RETURN_IF_ERROR(AppendRecord(tenant_id, session_id, record_payload));
  if (retention.empty()) {
    return absl::OkStatus();
  }
  // Sidecar is overwritten on each call: the policy applies to the whole log
  // going forward and the most recent value wins. Bucket-level Object Lock
  // remains the load-bearing immutability mechanism for synced objects; the
  // sidecar is advisory metadata for tooling, not the audit record itself.
  const std::filesystem::path sidecar =
      RetentionSidecarPathFor(tenant_id, session_id);
  std::shared_ptr<std::mutex> sidecar_mutex =
      PathMutexRegistry::Instance().Acquire(sidecar.string());
  std::lock_guard<std::mutex> guard(*sidecar_mutex);
  std::error_code error;
  std::filesystem::create_directories(sidecar.parent_path(), error);
  if (error) {
    return absl::InternalError(absl::StrCat(
        "Failed to create DPM retention sidecar directory: ", error.message()));
  }
  std::string body = absl::StrCat(
      "{\"retain_until_unix_seconds\":", retention.retain_until_unix_seconds,
      ",\"legal_hold\":", retention.legal_hold ? "true" : "false", "}\n");
  return DurablyWriteFile(sidecar, body);
}

absl::Status PosixEventSink::CreateBranch(
    absl::string_view parent_tenant_id, absl::string_view parent_session_id,
    absl::string_view branch_tenant_id, absl::string_view branch_session_id,
    uint64_t parent_record_count_at_branch) {
  RETURN_IF_ERROR(ValidateIdentity(parent_tenant_id, parent_session_id));
  RETURN_IF_ERROR(ValidateIdentity(branch_tenant_id, branch_session_id));
  if (parent_tenant_id == branch_tenant_id &&
      parent_session_id == branch_session_id) {
    return absl::InvalidArgumentError(
        "CreateBranch: branch identity must differ from parent identity.");
  }
  // The parent's records up to parent_record_count_at_branch must exist.
  // Scanning to validate is O(records-up-to-N); cheaper than bytes since we
  // only count, but the right answer for the structural-correctness goal.
  uint64_t parent_record_count = 0;
  RETURN_IF_ERROR(ForEachRecord(
      parent_tenant_id, parent_session_id,
      [&parent_record_count](absl::string_view) -> absl::Status {
        ++parent_record_count;
        return absl::OkStatus();
      }));
  if (parent_record_count_at_branch > parent_record_count) {
    return absl::InvalidArgumentError(absl::StrCat(
        "CreateBranch: parent_record_count_at_branch (",
        parent_record_count_at_branch, ") exceeds parent's record count (",
        parent_record_count, ")."));
  }

  const std::filesystem::path branch_log_path =
      PathFor(branch_tenant_id, branch_session_id);
  const std::filesystem::path branch_pointer_path =
      BranchPointerPathFor(branch_tenant_id, branch_session_id);

  // Serialize branch publication before checking for an existing branch.
  // Without this, same-process creators could all pass the exists check and
  // then atomically replace branch_pointer.json one after another.
  std::shared_ptr<std::mutex> path_mutex =
      PathMutexRegistry::Instance().Acquire(branch_pointer_path.string());
  std::lock_guard<std::mutex> guard(*path_mutex);

  // Reject if the branch identity is already in use.
  if (std::filesystem::exists(branch_log_path) ||
      std::filesystem::exists(branch_pointer_path)) {
    return absl::AlreadyExistsError(
        "CreateBranch: branch identity is already in use.");
  }

  std::error_code error;
  std::filesystem::create_directories(branch_pointer_path.parent_path(),
                                      error);
  if (error) {
    return absl::InternalError(absl::StrCat(
        "CreateBranch: failed to create branch directory: ", error.message()));
  }

  BranchPointer bp;
  bp.parent_tenant_id = std::string(parent_tenant_id);
  bp.parent_session_id = std::string(parent_session_id);
  bp.parent_record_count_at_branch = parent_record_count_at_branch;
  const std::string body = SerializeBranchPointer(bp);

  return DurablyCreateNewFile(branch_pointer_path, body);
}

absl::StatusOr<std::vector<std::string>> PosixEventSink::ReadRecords(
    absl::string_view tenant_id, absl::string_view session_id) const {
  std::vector<std::string> records;
  RETURN_IF_ERROR(ForEachRecord(
      tenant_id, session_id,
      [&records](absl::string_view record) -> absl::Status {
        records.emplace_back(record);
        return absl::OkStatus();
      }));
  return records;
}

namespace {

absl::StatusOr<absl::optional<BranchPointer>> ReadBranchPointer(
    const std::filesystem::path& branch_pointer_path) {
  if (!std::filesystem::exists(branch_pointer_path)) {
    return absl::optional<BranchPointer>();
  }
  std::ifstream in(branch_pointer_path, std::ios::in | std::ios::binary);
  if (!in.is_open()) {
    return absl::InternalError(
        absl::StrCat("Failed to read branch_pointer.json: ",
                     branch_pointer_path.string()));
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  ASSIGN_OR_RETURN(BranchPointer bp, ParseBranchPointer(buffer.str()));
  return absl::optional<BranchPointer>(std::move(bp));
}

absl::Status ForEachRecordImpl(
    const PosixEventSink& sink, absl::string_view tenant_id,
    absl::string_view session_id, int depth,
    uint64_t max_records,  // UINT64_MAX means "all"
    absl::FunctionRef<absl::Status(absl::string_view)> callback) {
  if (depth > kMaxBranchDepth) {
    return absl::FailedPreconditionError(absl::StrCat(
        "Branch depth exceeds kMaxBranchDepth (", kMaxBranchDepth, ")."));
  }
  // First, drain the parent's records (if this is a branch).
  uint64_t emitted = 0;
  ASSIGN_OR_RETURN(absl::optional<BranchPointer> bp,
                   ReadBranchPointer(sink.BranchPointerPathFor(tenant_id,
                                                               session_id)));
  if (bp.has_value()) {
    const uint64_t parent_limit = std::min(bp->parent_record_count_at_branch,
                                           max_records);
    uint64_t parent_emitted = 0;
    auto status = ForEachRecordImpl(
        sink, bp->parent_tenant_id, bp->parent_session_id, depth + 1,
        parent_limit,
        [&](absl::string_view record) -> absl::Status {
          ++parent_emitted;
          return callback(record);
        });
    if (!status.ok()) return status;
    emitted = parent_emitted;
    if (emitted >= max_records) return absl::OkStatus();
  }

  // Then emit this session's own records.
  const std::filesystem::path path = sink.PathFor(tenant_id, session_id);
  std::shared_ptr<std::mutex> path_mutex =
      PathMutexRegistry::Instance().Acquire(path.string());
  std::lock_guard<std::mutex> guard(*path_mutex);
  if (!std::filesystem::exists(path)) {
    return absl::OkStatus();
  }
  ASSIGN_OR_RETURN(LockedFile file,
                   LockedFile::Open(path, /*exclusive=*/false,
                                    /*create=*/false));
  ASSIGN_OR_RETURN(uint64_t file_size, file.Size());
  if (file_size == 0) {
    return absl::OkStatus();
  }

  ASSIGN_OR_RETURN(std::unique_ptr<MemoryMappedFile> mapped_file,
                   MemoryMappedFile::Create(path.string()));
  return ForEachMappedRecord(*mapped_file,
                             [&](absl::string_view record) -> absl::Status {
                               if (emitted >= max_records) {
                                 return absl::OkStatus();
                               }
                               ++emitted;
                               return callback(record);
                             });
}

}  // namespace

absl::Status PosixEventSink::ForEachRecord(
    absl::string_view tenant_id, absl::string_view session_id,
    absl::FunctionRef<absl::Status(absl::string_view)> callback) const {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  return ForEachRecordImpl(*this, tenant_id, session_id, /*depth=*/0,
                           /*max_records=*/UINT64_MAX, callback);
}

absl::StatusOr<EventSink::Generation> PosixEventSink::ProbeGeneration(
    absl::string_view tenant_id, absl::string_view session_id) const {
  RETURN_IF_ERROR(ValidateIdentity(tenant_id, session_id));
  EventSink::Generation gen;
  const std::filesystem::path path = PathFor(tenant_id, session_id);
  std::shared_ptr<std::mutex> path_mutex =
      PathMutexRegistry::Instance().Acquire(path.string());
  std::lock_guard<std::mutex> guard(*path_mutex);
  if (!std::filesystem::exists(path)) {
    return gen;
  }
  ASSIGN_OR_RETURN(LockedFile file,
                   LockedFile::Open(path, /*exclusive=*/false,
                                    /*create=*/false));
  ASSIGN_OR_RETURN(uint64_t file_size, file.Size());
  // file_size doubles as the opaque token: any append strictly grows the file
  // because every record is at least kLogMagic.size() + 8 + 1 bytes long. The
  // file lock plus the process-local mutex prevent torn reads of file_size
  // during a concurrent append.
  gen.opaque_token = file_size;
  // record_count is left at zero because computing it would require parsing;
  // consumers should treat (record_count, opaque_token) as an opaque pair.
  return gen;
}

}  // namespace litert::lm
