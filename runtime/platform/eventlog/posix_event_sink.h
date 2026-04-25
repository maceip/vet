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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_EVENTLOG_POSIX_EVENT_SINK_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_EVENTLOG_POSIX_EVENT_SINK_H_

#include <filesystem>
#include <string>
#include <vector>

#include "absl/functional/function_ref.h"  // from @com_google_absl
#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/eventlog/event_sink.h"

namespace litert::lm {

// EventSink backed by a POSIX-style filesystem rooted at root_path. Records
// for (tenant_id, session_id) are stored in
//   root_path / tenant_id / session_id / events.dpmlog
// using a magic header followed by length-prefixed records. Appends use
// inter-process file locking and fsync (or FlushFileBuffers on Windows).
// Reads use mmap.
//
// PosixEventSink holds no per-(tenant, session) cache of its own. Appends use
// a process-local path mutex plus an inter-process file lock; callers that
// need a decoded-event cache should layer it above the sink (see
// EventSourcedLog).
//
// The on-disk format is suitable for an S3 Files mount (NFS 4.x via
// mount -t s3files), an EFS mount, or local disk. See
// runtime/dpm/PHASE1_AUDIT_RECOVERY.md for the empirical verification.
class PosixEventSink : public EventSink {
 public:
  explicit PosixEventSink(std::filesystem::path root_path);

  absl::Status AppendRecord(absl::string_view tenant_id,
                            absl::string_view session_id,
                            absl::string_view record_payload) override;

  absl::Status AppendRecordWithRetention(
      absl::string_view tenant_id, absl::string_view session_id,
      absl::string_view record_payload,
      const RetentionPolicy& retention) override;

  // Path of the retention sidecar that PosixEventSink writes when a non-empty
  // RetentionPolicy is supplied. The sidecar is JSON of the form
  //   {"retain_until_unix_seconds": <int>, "legal_hold": <bool>}
  // alongside events.dpmlog. Bucket-level Object Lock applies regardless.
  std::filesystem::path RetentionSidecarPathFor(
      absl::string_view tenant_id, absl::string_view session_id) const;

  absl::StatusOr<std::vector<std::string>> ReadRecords(
      absl::string_view tenant_id,
      absl::string_view session_id) const override;

  absl::Status ForEachRecord(
      absl::string_view tenant_id, absl::string_view session_id,
      absl::FunctionRef<absl::Status(absl::string_view)> callback)
      const override;

  absl::StatusOr<EventSink::Generation> ProbeGeneration(
      absl::string_view tenant_id,
      absl::string_view session_id) const override;

  std::filesystem::path PathFor(absl::string_view tenant_id,
                                absl::string_view session_id) const;

  // Creates a copy-on-write branch at the substrate level. After this call:
  //   - (branch_tenant_id, branch_session_id) is a new, addressable session.
  //   - Reads on the branch see the first parent_record_count_at_branch
  //     records of (parent_tenant_id, parent_session_id) followed by any
  //     records appended to the branch itself.
  //   - Appends to the branch never modify the parent.
  //   - The operation is O(1) in the parent's record count: only a small
  //     branch_pointer.json sidecar is written; the parent's bytes are not
  //     copied.
  //
  // Returns InvalidArgument if either identity has bad components, if the
  // parent does not exist, if parent_record_count_at_branch exceeds the
  // parent's current record count, or if the branch identity already
  // exists. Branches of branches are supported up to a fixed depth bound
  // (kMaxBranchDepth, 16).
  absl::Status CreateBranch(absl::string_view parent_tenant_id,
                            absl::string_view parent_session_id,
                            absl::string_view branch_tenant_id,
                            absl::string_view branch_session_id,
                            uint64_t parent_record_count_at_branch);

  std::filesystem::path BranchPointerPathFor(
      absl::string_view tenant_id, absl::string_view session_id) const;

 private:
  std::filesystem::path root_path_;
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_EVENTLOG_POSIX_EVENT_SINK_H_
