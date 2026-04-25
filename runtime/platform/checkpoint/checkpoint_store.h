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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_CHECKPOINT_STORE_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_CHECKPOINT_STORE_H_

#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {

// Content-addressed checkpoint storage. Implementations own the on-wire /
// at-rest representation; consumers supply the ABI proto bytes and the
// payload bytes; the store binds them together by computing the payload's
// hash and using it as the address.
//
// On Put, body_hash is computed by the store using the algorithm declared
// in the ABI bytes (BLAKE3 default, SHA-256 fallback). The store then
// writes the (abi_bytes, payload_bytes) pair under that hash. Subsequent
// Get-by-hash returns the same bytes (and rejects with DataLoss if a hash
// mismatch is detected).
//
// The proto wrapping CheckpointAbi+CheckpointAnnotations should already be
// serialized by the caller; the store does not link against the proto
// generated code, so it can stay free of the protobuf dependency at the
// substrate layer. (The DPM layer that calls the store has the proto.)
class CheckpointStore {
 public:
  virtual ~CheckpointStore() = default;

  // Stores a checkpoint and returns its content address. body_hash is
  // computed by the store using `algo`, regardless of what is in the
  // serialized ABI bytes (the caller is responsible for keeping them in
  // sync).
  virtual absl::StatusOr<Hash256> Put(absl::string_view tenant_id,
                                      absl::string_view session_id,
                                      absl::string_view abi_bytes,
                                      absl::string_view payload_bytes,
                                      HashAlgorithm algo) = 0;

  // Retrieved checkpoint. The body_hash recorded by Put is recomputed and
  // compared against `address`; DataLoss is returned on mismatch.
  struct CheckpointBlob {
    std::string abi_bytes;
    std::string payload_bytes;
    HashAlgorithm algorithm = HashAlgorithm::kBlake3;
  };
  virtual absl::StatusOr<CheckpointBlob> Get(absl::string_view tenant_id,
                                             absl::string_view session_id,
                                             const Hash256& address) const = 0;

  virtual absl::StatusOr<bool> Exists(absl::string_view tenant_id,
                                      absl::string_view session_id,
                                      const Hash256& address) const = 0;

  // Lists all checkpoint addresses for a (tenant, session). Order is
  // implementation-defined; consumers that need a chain order should walk
  // parent_hashes via runtime/platform/provenance.
  virtual absl::StatusOr<std::vector<Hash256>> List(
      absl::string_view tenant_id, absl::string_view session_id) const = 0;
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_CHECKPOINT_STORE_H_
