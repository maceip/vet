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

// Content-addressed checkpoint storage with **two** independent address
// spaces:
//
//   1. Payload blobs are addressed by body_hash = HashBytes(payload).
//      Two checkpoints with the same KV bytes but different metadata
//      (e.g. branched on a different parent) share one payload blob.
//
//   2. Manifest records are addressed by manifest_hash, the digest the
//      DPM layer has already computed over the proto-encoded
//      CheckpointAbi (see runtime/platform/checkpoint/canonical_manifest).
//      The manifest record is the (abi_bytes, body_hash) pair; the
//      Merkle DAG node identity is the manifest_hash, not body_hash.
//
// The split fixes the prior shape, where the store keyed the whole
// (abi, payload) blob by body_hash and rejected legitimate cases like
// "same KV bytes, different parent_hashes" as a manifest collision.
//
// PutPayload and PutManifest are independent. A typical commit flow:
//
//   const Hash256 body_hash = store->PutPayload(tenant, session,
//                                               payload_bytes, algo);
//   ... caller fills CheckpointAbi.body_hash with body_hash, computes
//       manifest_hash via canonical_manifest, then ...
//   store->PutManifest(tenant, session, manifest_hash,
//                       abi_bytes, body_hash);
//
// Both Put operations are idempotent on identical content (same bytes
// at the same address is OK). A second Put under the same address with
// different bytes returns DataLoss — that's the structural detection
// path for hash collisions or torn-write artifacts.
class CheckpointStore {
 public:
  virtual ~CheckpointStore() = default;

  // ------------------------------------------------------------------
  // Payload (body) blobs.
  // ------------------------------------------------------------------

  // Writes the payload bytes and returns body_hash = HashBytes(payload).
  // Idempotent: repeated calls with identical bytes return the same
  // address. Different bytes at an existing address returns DataLoss
  // (which would indicate either a hash collision or filesystem
  // corruption).
  virtual absl::StatusOr<Hash256> PutPayload(absl::string_view tenant_id,
                                             absl::string_view session_id,
                                             absl::string_view payload_bytes,
                                             HashAlgorithm algo) = 0;

  virtual absl::StatusOr<std::string> GetPayload(
      absl::string_view tenant_id, absl::string_view session_id,
      const Hash256& body_hash) const = 0;

  virtual absl::StatusOr<bool> PayloadExists(
      absl::string_view tenant_id, absl::string_view session_id,
      const Hash256& body_hash) const = 0;

  // ------------------------------------------------------------------
  // Manifest records.
  // ------------------------------------------------------------------

  // Stores a manifest record under manifest_hash. The caller has already
  // computed manifest_hash over the canonical encoding of abi_bytes (see
  // canonical_manifest.h); the store does not recompute the digest
  // because the canonical encoding is a DPM-layer concern.
  //
  // referenced_body_hash is the body_hash inside abi_bytes; the store
  // records it alongside so consumers can resolve the payload without
  // re-parsing the proto.
  virtual absl::Status PutManifest(absl::string_view tenant_id,
                                   absl::string_view session_id,
                                   const Hash256& manifest_hash,
                                   absl::string_view abi_bytes,
                                   const Hash256& referenced_body_hash) = 0;

  struct ManifestRecord {
    std::string abi_bytes;
    Hash256 referenced_body_hash;
  };
  virtual absl::StatusOr<ManifestRecord> GetManifest(
      absl::string_view tenant_id, absl::string_view session_id,
      const Hash256& manifest_hash) const = 0;

  virtual absl::StatusOr<bool> ManifestExists(
      absl::string_view tenant_id, absl::string_view session_id,
      const Hash256& manifest_hash) const = 0;

  virtual absl::StatusOr<std::vector<Hash256>> ListManifests(
      absl::string_view tenant_id, absl::string_view session_id) const = 0;
};

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_PLATFORM_CHECKPOINT_CHECKPOINT_STORE_H_
