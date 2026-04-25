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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_LOCAL_FILESYSTEM_CHECKPOINT_STORE_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_LOCAL_FILESYSTEM_CHECKPOINT_STORE_H_

#include <filesystem>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/checkpoint/checkpoint_store.h"
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {

// CheckpointStore backed by a POSIX-style filesystem with two independent
// content-addressed address spaces:
//
//   root / tenant / session / payloads  / <body_hash>.dpmpayload
//   root / tenant / session / manifests / <manifest_hash>.dpmmanifest
//
// Payload file framing:
//   magic "DPMPAYLD1\n" | size:u64 | bytes
//
// Manifest file framing:
//   magic "DPMMANI1\n" | abi_size:u32 | abi_bytes | body_hash:32 raw bytes
//
// Both Put operations write through DurablyWriteFile (atomic temp +
// fsync + rename + dir-fsync). Idempotent puts content-verify; mismatch
// at an existing address returns DataLoss.
class LocalFilesystemCheckpointStore : public CheckpointStore {
 public:
  explicit LocalFilesystemCheckpointStore(std::filesystem::path root_path);

  // Payload blobs.
  absl::StatusOr<Hash256> PutPayload(absl::string_view tenant_id,
                                     absl::string_view session_id,
                                     absl::string_view payload_bytes,
                                     HashAlgorithm algo) override;
  absl::StatusOr<std::string> GetPayload(
      absl::string_view tenant_id, absl::string_view session_id,
      const Hash256& body_hash) const override;
  absl::StatusOr<bool> PayloadExists(
      absl::string_view tenant_id, absl::string_view session_id,
      const Hash256& body_hash) const override;

  // Manifest records.
  absl::Status PutManifest(absl::string_view tenant_id,
                           absl::string_view session_id,
                           const Hash256& manifest_hash,
                           absl::string_view abi_bytes,
                           const Hash256& referenced_body_hash) override;
  absl::StatusOr<ManifestRecord> GetManifest(
      absl::string_view tenant_id, absl::string_view session_id,
      const Hash256& manifest_hash) const override;
  absl::StatusOr<bool> ManifestExists(
      absl::string_view tenant_id, absl::string_view session_id,
      const Hash256& manifest_hash) const override;
  absl::StatusOr<std::vector<Hash256>> ListManifests(
      absl::string_view tenant_id,
      absl::string_view session_id) const override;

  std::filesystem::path PayloadPathFor(absl::string_view tenant_id,
                                       absl::string_view session_id,
                                       const Hash256& body_hash) const;
  std::filesystem::path ManifestPathFor(absl::string_view tenant_id,
                                        absl::string_view session_id,
                                        const Hash256& manifest_hash) const;

 private:
  std::filesystem::path root_path_;
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_LOCAL_FILESYSTEM_CHECKPOINT_STORE_H_
