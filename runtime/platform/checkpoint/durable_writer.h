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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_DURABLE_WRITER_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_DURABLE_WRITER_H_

#include <filesystem>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl

namespace litert::lm {

// Writes `bytes` to `target_path` durably:
//   1. Open <target>.tmp.<unique> with O_CREAT|O_TRUNC|O_WRONLY (or
//      Windows equivalent).
//   2. Write all bytes; loop on partial writes.
//   3. fsync the file descriptor (FlushFileBuffers on Windows).
//   4. Close.
//   5. rename(tmp, target) — atomic on a single filesystem.
//   6. fsync the parent directory (no-op on Windows where the directory
//      is not a regular file but the rename itself is committed by the
//      filesystem on close).
//
// Returns OK only after step 6 completes. If the process dies after step
// 4 the target either has the new bytes or the old bytes — never a torn
// half-write. Stale temp files from crashed writers are easy to GC by
// matching the suffix.
absl::Status DurablyWriteFile(const std::filesystem::path& target_path,
                              absl::string_view bytes);

// Reads the file at `path` if it exists. Returns an empty string and OK
// status when the file is missing. Used by idempotent Put paths to verify
// existing content before declaring a no-op.
absl::Status ReadEntireFileIfExists(const std::filesystem::path& path,
                                    std::string* out);

// Creates `target_path` exclusively and writes `bytes` durably:
//   1. Write and fsync a unique temp file in the target directory.
//   2. Publish with no-replace semantics (link on POSIX, MoveFileEx
//      without replace on Windows). If the target already exists, the
//      filesystem itself serializes the create and this returns
//      AlreadyExists.
//   3. fsync the parent directory on POSIX so the new directory entry is
//      committed.
//
// Use this for sidecars and other "publish-once" records where the
// non-atomic exists-check + atomic-overwrite-rename pattern of
// DurablyWriteFile is structurally wrong (concurrent creators would
// both pass the exists-check and last-writer-wins).
//
// Returns AlreadyExists when the target already exists, even if the
// existing bytes match what we would have written. Idempotent retries
// can be implemented by a caller that reads the existing bytes and
// compares; the primitive itself stays strict.
absl::Status DurablyCreateNewFile(const std::filesystem::path& target_path,
                                  absl::string_view bytes);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_DURABLE_WRITER_H_
