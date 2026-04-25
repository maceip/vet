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

#include "runtime/dpm/checkpoint_policy.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/dpm/event.h"
#include "runtime/platform/checkpoint/kv_quantization.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/proto/checkpoint.pb.h"

namespace litert::lm {
namespace {

ThawDecision Refill(absl::string_view reason) {
  ThawDecision d;
  d.can_thaw = false;
  d.must_refill_from_log = true;
  d.reason = std::string(reason);
  return d;
}

bool HashFromProto(const proto::Hash256& proto, Hash256* out) {
  if (proto.bytes().size() != out->bytes.size()) return false;
  std::memcpy(out->bytes.data(), proto.bytes().data(), out->bytes.size());
  return true;
}

KvDtype KvDtypeFromProto(proto::KvDtype proto) {
  switch (proto) {
    case proto::KV_DTYPE_FP16:
      return KvDtype::kFp16;
    case proto::KV_DTYPE_INT8_PER_TOKEN:
      return KvDtype::kInt8PerToken;
    case proto::KV_DTYPE_INT4_CHANNEL:
      return KvDtype::kInt4Channel;
    default:
      // Unspecified is treated as fp16 for the safe-by-default rule.
      return KvDtype::kFp16;
  }
}

}  // namespace

ThawDecision EvaluateCheckpointCompatibility(
    const proto::CheckpointAbi& manifest, const ThawRequest& request) {
  // Identity: tenant / session / branch must match exactly.
  if (manifest.identity().tenant_id() != request.tenant_id ||
      manifest.identity().session_id() != request.session_id ||
      manifest.identity().branch_id() != request.branch_id) {
    return Refill("tenant, session, or branch mismatch");
  }

  // Model identity: model_id, artifact_hash, and class.
  if (manifest.model().model_id() != request.model_id) {
    return Refill("model_id mismatch");
  }
  Hash256 manifest_artifact_hash;
  if (!HashFromProto(manifest.model().artifact_hash(),
                     &manifest_artifact_hash)) {
    return Refill("malformed model.artifact_hash on manifest");
  }
  if (!(manifest_artifact_hash == request.model_artifact_hash)) {
    return Refill("model artifact hash mismatch");
  }
  if (request.model_class != proto::MODEL_CLASS_UNSPECIFIED &&
      manifest.model().model_class() != request.model_class) {
    return Refill("model class mismatch");
  }

  // Architecture: tag must be exactly the local host's tag. Cross-arch
  // soft fallback flows back through must_refill_from_log so the caller
  // can choose to re-prefill from the event log.
  if (manifest.producer().architecture_tag() != request.architecture_tag) {
    return Refill("architecture_tag mismatch");
  }

  // KV dtype: must satisfy the local replay-safety policy.
  const KvDtype manifest_dtype = KvDtypeFromProto(manifest.kv_dtype());
  if (auto status =
          RequireReplaySafeKvDtype(request.kv_policy, manifest_dtype);
      !status.ok()) {
    return Refill(status.message());
  }

  ThawDecision ok;
  ok.can_thaw = true;
  ok.must_refill_from_log = false;
  ok.reason = "checkpoint is thaw-compatible";
  return ok;
}

ThawDecision EvaluateCheckpointThawVerification(const Hash256& expected,
                                                const Hash256& actual) {
  static const Hash256 kZero;  // all-zero default
  if (expected == kZero || actual == kZero) {
    return Refill("missing thaw verification digest");
  }
  if (!(expected == actual)) {
    return Refill("manifest digest mismatch");
  }
  ThawDecision ok;
  ok.can_thaw = true;
  ok.must_refill_from_log = false;
  ok.reason = "manifest digest verified";
  return ok;
}

CheckpointTriggerDecision ShouldCreateCheckpoint(
    const CheckpointTriggerPolicy& policy,
    const CheckpointTriggerState& state) {
  CheckpointTriggerDecision out;
  if (policy.checkpoint_on_handoff && state.explicit_handoff) {
    out.should_checkpoint = true;
    out.trigger = proto::TRIGGER_HANDOFF;
    out.reason = "explicit handoff";
    return out;
  }
  if (policy.checkpoint_on_correction &&
      state.last_event_type == Event::Type::kCorrection) {
    out.should_checkpoint = true;
    out.trigger = proto::TRIGGER_HANDOFF;  // correction-driven boundary
    out.reason = "correction boundary";
    return out;
  }
  if (state.tokens_since_checkpoint >= policy.max_delta_tokens) {
    out.should_checkpoint = true;
    out.trigger = proto::TRIGGER_TOKEN_THRESHOLD;
    out.reason = "max token delta";
    return out;
  }
  if (state.max_context_tokens > 0 &&
      static_cast<double>(state.current_context_tokens) /
              static_cast<double>(state.max_context_tokens) >=
          policy.context_pressure_ratio) {
    out.should_checkpoint = true;
    out.trigger = proto::TRIGGER_TOKEN_THRESHOLD;
    out.reason = "context pressure";
    return out;
  }
  if (policy.checkpoint_on_milestone_tool &&
      state.last_event_was_milestone_tool &&
      state.tokens_since_checkpoint >= policy.min_delta_tokens) {
    out.should_checkpoint = true;
    out.trigger = proto::TRIGGER_MILESTONE_TOOL;
    out.reason = "milestone tool";
    return out;
  }
  if (policy.checkpoint_on_idle && state.user_idle &&
      state.tokens_since_checkpoint > 0) {
    out.should_checkpoint = true;
    out.speculative = true;
    out.trigger = proto::TRIGGER_IDLE_SPECULATIVE;
    out.reason = "user idle";
    return out;
  }
  return out;
}

proto::CheckpointTransportTier SelectCheckpointTransportTier(
    const CheckpointTransportRequest& request) {
  if (request.rack_local && request.rdma_available &&
      !request.cross_az_or_region) {
    return proto::TRANSPORT_TIER_RDMA_ROCE;
  }
  return proto::TRANSPORT_TIER_GRPC_FLATBUFFERS;
}

bool ShouldCompactCheckpointDeltas(
    const std::vector<proto::CheckpointAbi>& delta_chain,
    const CheckpointCompactionPolicy& policy) {
  int delta_levels = 0;
  int64_t delta_bytes = 0;
  for (const proto::CheckpointAbi& abi : delta_chain) {
    if (abi.level().level() == 0) continue;
    ++delta_levels;
    delta_bytes += abi.body_size_bytes();
  }
  return delta_levels >= policy.max_delta_levels ||
         delta_bytes >= policy.max_delta_bytes;
}

absl::Status ValidateCheckpointStorageTiers(
    const proto::CheckpointStorageBinding& binding) {
  if (binding.metadata_tier() != proto::STORAGE_TIER_MEMORYDB_METADATA) {
    return absl::InvalidArgumentError(
        "checkpoint metadata must live in the hot metadata tier "
        "(MemoryDB).");
  }
  if (binding.blob_tier() == proto::STORAGE_TIER_MEMORYDB_METADATA) {
    return absl::InvalidArgumentError(
        "checkpoint blobs must not be stored in the metadata tier; "
        "use S3 Express One Zone or the local file tier.");
  }
  if (binding.metadata_uri().empty()) {
    return absl::InvalidArgumentError(
        "CheckpointStorageBinding.metadata_uri is required.");
  }
  if (binding.blob_uri().empty()) {
    return absl::InvalidArgumentError(
        "CheckpointStorageBinding.blob_uri is required.");
  }
  return absl::OkStatus();
}

}  // namespace litert::lm
