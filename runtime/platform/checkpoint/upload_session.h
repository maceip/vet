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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_UPLOAD_SESSION_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_UPLOAD_SESSION_H_

#include <cstdint>
#include <string>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/checkpoint/checkpoint_store.h"
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {

// Two-phase commit upload state machine for streaming a checkpoint payload
// in ordered byte ranges. The state machine is transport-agnostic — gRPC,
// RDMA, or local mmap copy can all drive it — and on Finalize it persists
// the assembled bytes via a CheckpointStore::Put, which is itself durable
// (atomic temp+rename+fsync) and content-addressed.
//
// Lifecycle:
//   1. construct (CheckpointUploadSession)
//   2. BeginManifest(declared_payload_size_bytes, abi_bytes, algo)
//   3. AddFrame(offset, bytes) once per frame, in offset order; the
//      session checks contiguity, accumulates the bytes in order, and
//      records each frame's hash for the manifest record.
//   4. Finalize(): verifies declared byte count matches received; calls
//      CheckpointStore::Put which durably writes the payload bytes
//      (atomic temp+rename+fsync) and returns the content-address.
//      The session then exits the kFrames phase and exposes the hash.
//
// The session does not perform any I/O between Begin and Finalize; bytes
// live in memory until commit. Callers that want spill-to-disk for very
// large payloads can wrap a custom CheckpointStore that streams to
// temporary storage, but the contract here is "buffer in memory, commit
// at the end."
class CheckpointUploadSession {
 public:
  // The store is borrowed; the caller owns it and keeps it alive for the
  // duration of the session.
  CheckpointUploadSession(absl::string_view tenant_id,
                          absl::string_view session_id,
                          CheckpointStore* store);

  // Declares the payload's expected size and the ABI bytes to commit
  // alongside the body. May only be called once, before any frame.
  absl::Status BeginManifest(uint64_t declared_payload_size_bytes,
                             absl::string_view abi_bytes, HashAlgorithm algo);

  // Appends one ordered, contiguous frame. offset must equal the running
  // accumulator so out-of-order or overlapping frames are rejected with
  // InvalidArgument. AddFrame may be called many times; each call
  // accumulates bytes and records its frame hash.
  absl::Status AddFrame(uint64_t offset, absl::string_view bytes);

  // Verifies the declared byte count was received, then commits via
  // CheckpointStore::Put. Returns the content-address Hash256.
  absl::StatusOr<Hash256> Finalize();

  bool finalized() const { return phase_ == Phase::kFinalized; }
  uint64_t bytes_received() const { return next_offset_; }

 private:
  enum class Phase {
    kReady,
    kManifestBegun,
    kFrames,
    kFinalized,
  };

  std::string tenant_id_;
  std::string session_id_;
  CheckpointStore* store_;

  Phase phase_ = Phase::kReady;
  uint64_t declared_size_ = 0;
  uint64_t next_offset_ = 0;
  HashAlgorithm algo_ = HashAlgorithm::kBlake3;
  std::string abi_bytes_;
  std::string payload_buffer_;
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_UPLOAD_SESSION_H_
