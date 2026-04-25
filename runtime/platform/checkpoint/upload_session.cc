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
#include "runtime/util/status_macros.h"

namespace litert::lm {

CheckpointUploadSession::CheckpointUploadSession(
    absl::string_view tenant_id, absl::string_view session_id,
    CheckpointStore* store)
    : tenant_id_(tenant_id), session_id_(session_id), store_(store) {}

absl::Status CheckpointUploadSession::BeginManifest(
    const ManifestMeta& meta) {
  if (phase_ != Phase::kReady) {
    return absl::FailedPreconditionError(
        "CheckpointUploadSession: BeginManifest already called.");
  }
  if (store_ == nullptr) {
    return absl::FailedPreconditionError(
        "CheckpointUploadSession: store is null.");
  }
  if (meta.declared_payload_size_bytes == 0) {
    return absl::InvalidArgumentError(
        "CheckpointUploadSession: declared_payload_size_bytes must be > 0.");
  }
  if (meta.abi_bytes.empty()) {
    return absl::InvalidArgumentError(
        "CheckpointUploadSession: abi_bytes must be non-empty.");
  }
  // Reject all-zero hashes; they would silently bypass verification at
  // commit time. Producers must pre-compute body_hash and manifest_hash
  // before the upload begins.
  static const Hash256 kZero;
  if (meta.expected_body_hash == kZero) {
    return absl::InvalidArgumentError(
        "CheckpointUploadSession: expected_body_hash is zero; the "
        "producer must pre-compute the body hash before BeginManifest.");
  }
  if (meta.manifest_hash == kZero) {
    return absl::InvalidArgumentError(
        "CheckpointUploadSession: manifest_hash is zero; the DPM layer "
        "must compute it via canonical_manifest before BeginManifest.");
  }
  meta_ = meta;
  payload_buffer_.clear();
  payload_buffer_.reserve(meta.declared_payload_size_bytes);
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
  if (next_offset_ + bytes.size() > meta_.declared_payload_size_bytes) {
    return absl::InvalidArgumentError(absl::StrCat(
        "CheckpointUploadSession: frame would exceed declared size (",
        meta_.declared_payload_size_bytes, " bytes)."));
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
  if (next_offset_ != meta_.declared_payload_size_bytes) {
    return absl::DataLossError(absl::StrCat(
        "CheckpointUploadSession: declared ",
        meta_.declared_payload_size_bytes, " bytes but received ",
        next_offset_, "."));
  }

  // Verify the assembled bytes hash to the manifest's expected
  // body_hash. No Put happens until verification passes.
  const Hash256 actual_body_hash =
      HashBytes(meta_.algo, payload_buffer_);
  if (!(actual_body_hash == meta_.expected_body_hash)) {
    return absl::DataLossError(absl::StrCat(
        "CheckpointUploadSession: body_hash mismatch on Finalize "
        "(expected ", meta_.expected_body_hash.ToHex(),
        ", got ", actual_body_hash.ToHex(),
        "). Refusing to commit."));
  }

  ASSIGN_OR_RETURN(Hash256 stored_body_hash,
                   store_->PutPayload(tenant_id_, session_id_,
                                      payload_buffer_, meta_.algo));
  if (!(stored_body_hash == meta_.expected_body_hash)) {
    // PutPayload re-derives body_hash from the bytes; if that disagrees
    // with our verification above the store is using a different
    // algorithm than meta_.algo. Refuse the manifest write so we never
    // leave a manifest pointing at the wrong payload.
    return absl::DataLossError(
        "CheckpointUploadSession: PutPayload returned a body_hash "
        "that disagrees with the verified expected_body_hash; the "
        "store and manifest hash algorithms are out of sync.");
  }
  RETURN_IF_ERROR(store_->PutManifest(tenant_id_, session_id_,
                                      meta_.manifest_hash, meta_.abi_bytes,
                                      meta_.expected_body_hash));
  phase_ = Phase::kFinalized;
  return meta_.manifest_hash;
}

}  // namespace litert::lm
