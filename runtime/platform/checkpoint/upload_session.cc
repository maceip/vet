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

#include "runtime/platform/checkpoint/upload_session.h"

#include <cstdint>
#include <string>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/checkpoint/checkpoint_store.h"
#include "runtime/platform/hash/hasher.h"

namespace litert::lm {

CheckpointUploadSession::CheckpointUploadSession(
    absl::string_view tenant_id, absl::string_view session_id,
    CheckpointStore* store)
    : tenant_id_(tenant_id), session_id_(session_id), store_(store) {}

absl::Status CheckpointUploadSession::BeginManifest(
    uint64_t declared_payload_size_bytes, absl::string_view abi_bytes,
    HashAlgorithm algo) {
  if (phase_ != Phase::kReady) {
    return absl::FailedPreconditionError(
        "CheckpointUploadSession: BeginManifest already called.");
  }
  if (store_ == nullptr) {
    return absl::FailedPreconditionError(
        "CheckpointUploadSession: store is null.");
  }
  if (declared_payload_size_bytes == 0) {
    return absl::InvalidArgumentError(
        "CheckpointUploadSession: declared_payload_size_bytes must be > 0.");
  }
  declared_size_ = declared_payload_size_bytes;
  abi_bytes_.assign(abi_bytes.data(), abi_bytes.size());
  algo_ = algo;
  payload_buffer_.clear();
  payload_buffer_.reserve(declared_payload_size_bytes);
  phase_ = Phase::kManifestBegun;
  return absl::OkStatus();
}

absl::Status CheckpointUploadSession::AddFrame(uint64_t offset,
                                               absl::string_view bytes) {
  if (phase_ != Phase::kManifestBegun && phase_ != Phase::kFrames) {
    return absl::FailedPreconditionError(
        "CheckpointUploadSession: AddFrame outside manifest/frames phase.");
  }
  if (offset != next_offset_) {
    return absl::InvalidArgumentError(absl::StrCat(
        "CheckpointUploadSession: frames must be contiguous and ordered "
        "(expected offset ",
        next_offset_, ", got ", offset, ")."));
  }
  if (bytes.empty()) {
    return absl::InvalidArgumentError(
        "CheckpointUploadSession: empty frame.");
  }
  if (next_offset_ + bytes.size() > declared_size_) {
    return absl::InvalidArgumentError(absl::StrCat(
        "CheckpointUploadSession: frame would exceed declared size (",
        declared_size_, " bytes)."));
  }
  payload_buffer_.append(bytes.data(), bytes.size());
  next_offset_ += bytes.size();
  phase_ = Phase::kFrames;
  return absl::OkStatus();
}

absl::StatusOr<Hash256> CheckpointUploadSession::Finalize() {
  if (phase_ != Phase::kFrames) {
    return absl::FailedPreconditionError(
        "CheckpointUploadSession: Finalize before any frames received.");
  }
  if (next_offset_ != declared_size_) {
    return absl::DataLossError(absl::StrCat(
        "CheckpointUploadSession: declared ", declared_size_,
        " bytes but received ", next_offset_, "."));
  }
  // Commit through the durable store. The store does its own atomic
  // temp+rename+fsync and content-addressing, so partial-failure recovery
  // is the store's responsibility.
  auto status = store_->Put(tenant_id_, session_id_, abi_bytes_,
                            payload_buffer_, algo_);
  if (!status.ok()) return status.status();
  phase_ = Phase::kFinalized;
  return *status;
}

}  // namespace litert::lm
