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

// Two-phase commit upload state machine for streaming a checkpoint
// payload in ordered byte ranges. Transport-agnostic: gRPC, RDMA, or
// local mmap copy can all drive it.
//
// Verification contract (load-bearing):
//   On Finalize, the assembled bytes are hashed and compared against
//   ManifestMeta.expected_body_hash. Mismatch returns DataLoss without
//   any Put. This is the difference between "the manifest claims X
//   bytes arrived" and "we verified that X bytes arrived and the bytes
//   match the manifest's body_hash before we committed."
//
// Lifecycle:
//   1. ctor.
//   2. BeginManifest(meta) — declares the manifest record being
//      committed: expected payload size, expected body_hash, algorithm,
//      ABI bytes, and pre-computed manifest_hash.
//   3. AddFrame(offset, bytes) one or more times, in ordered contiguous
//      offset order.
//   4. Finalize() — verifies (a) declared bytes received and (b)
//      HashBytes(buffer) == expected_body_hash. On match, calls
//      CheckpointStore::PutPayload then PutManifest. Returns the
//      manifest_hash on success.
class CheckpointUploadSession {
 public:
  CheckpointUploadSession(absl::string_view tenant_id,
                          absl::string_view session_id,
                          CheckpointStore* store);

  struct ManifestMeta {
    // Expected size of payload bytes the upload will deliver.
    uint64_t declared_payload_size_bytes = 0;
    // Expected body_hash, pre-computed by the producer over the bytes
    // that will be streamed. The session refuses to commit if the
    // assembled bytes don't hash to this value.
    Hash256 expected_body_hash;
    // Hash algorithm used for both body_hash recomputation and the
    // payload Put.
    HashAlgorithm algo = HashAlgorithm::kBlake3;
    // The proto-encoded CheckpointAbi bytes.
    std::string abi_bytes;
    // Pre-computed manifest_hash over the canonical encoding of
    // abi_bytes (see canonical_manifest.h). The session does not
    // recompute it here — that is the DPM layer's concern — but it
    // is recorded with the manifest record so PutManifest can
    // content-address by it.
    Hash256 manifest_hash;
  };

  absl::Status BeginManifest(const ManifestMeta& meta);

  // Appends one ordered, contiguous frame.
  absl::Status AddFrame(uint64_t offset, absl::string_view bytes);

  // Verifies declared bytes received and HashBytes(buffer) ==
  // expected_body_hash, then commits via PutPayload + PutManifest.
  // Returns the manifest_hash on success.
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
  ManifestMeta meta_;
  uint64_t next_offset_ = 0;
  std::string payload_buffer_;
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_UPLOAD_SESSION_H_
