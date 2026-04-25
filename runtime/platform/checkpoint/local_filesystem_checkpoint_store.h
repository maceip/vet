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

// CheckpointStore backed by a POSIX-style filesystem. Layout:
//   root_path / tenant_id / session_id / <hex_address>.dpmckpt
//
// The .dpmckpt file is framed:
//   magic "DPMSTORE1\n"
//   abi_size: u32 le
//   abi_bytes: bytes
//   payload_size: u64 le
//   payload_bytes: bytes
//
// body_hash is recomputed on every Get and the result is compared against
// the requested address. Mismatch returns DataLoss.
//
// This backend is suitable for development, tests, and Phase 1-style
// S3 Files mounts (where the bucket plus Object Lock provides durability).
// A Phase 2.2 partner can implement S3ExpressCheckpointStore by following
// the same interface.
class LocalFilesystemCheckpointStore : public CheckpointStore {
 public:
  explicit LocalFilesystemCheckpointStore(std::filesystem::path root_path);

  absl::StatusOr<Hash256> Put(absl::string_view tenant_id,
                              absl::string_view session_id,
                              absl::string_view abi_bytes,
                              absl::string_view payload_bytes,
                              HashAlgorithm algo) override;

  absl::StatusOr<CheckpointBlob> Get(absl::string_view tenant_id,
                                     absl::string_view session_id,
                                     const Hash256& address) const override;

  absl::StatusOr<bool> Exists(absl::string_view tenant_id,
                              absl::string_view session_id,
                              const Hash256& address) const override;

  absl::StatusOr<std::vector<Hash256>> List(
      absl::string_view tenant_id,
      absl::string_view session_id) const override;

  std::filesystem::path PathFor(absl::string_view tenant_id,
                                absl::string_view session_id,
                                const Hash256& address) const;

 private:
  std::filesystem::path root_path_;
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_LOCAL_FILESYSTEM_CHECKPOINT_STORE_H_
