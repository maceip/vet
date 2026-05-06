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

#ifndef THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_CHECKPOINT_POLICY_H_
#define THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_CHECKPOINT_POLICY_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/dpm/event.h"
#include "runtime/platform/checkpoint/kv_quantization.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/proto/checkpoint.pb.h"

namespace litert::lm {

// Policy primitives for Phase 2 checkpoints. These are the runtime-side
// decisions a deployment makes about whether a checkpoint is thaw-safe,
// when to create one, which transport tier to use, and when to compact a
// delta chain. Each function consumes the strongly-typed CheckpointAbi
// proto from runtime/proto/checkpoint.proto and returns a decision the
// caller acts on. The substrate-side codecs / stores / DAG do not call
// these directly.
//
// All policy functions are pure: same inputs, same decision. Logging and
// metrics are the caller's job.

// What the runtime knows about the live request that wants to thaw a
// checkpoint. This is the local side of the (manifest, request)
// compatibility comparison.
struct ThawRequest {
  std::string tenant_id;
  std::string session_id;
  std::string branch_id;

  // Pinned model_id and artifact hash (BLAKE3) the loaded inference stack
  // is bound to. A checkpoint that references different bytes is
  // automatically incompatible.
  std::string model_id;
  Hash256 model_artifact_hash;
  proto::ModelClass model_class = proto::MODEL_CLASS_UNSPECIFIED;

  // Architecture tag of the local host, e.g. "arm64-hexagon-int8".
  std::string architecture_tag;

  // The deployment's KV-dtype policy. require_replay_safe rejects any
  // manifest whose kv_dtype is not kFp16 even when the architecture
  // matches; see runtime/platform/checkpoint/kv_quantization.h.
  KvDtypePolicy kv_policy;
};

// Outcome of a compatibility check. must_refill_from_log is the soft
// fallback the structure doc requires: the runtime can re-prefill from
// the event log when the checkpoint isn't thaw-safe.
struct ThawDecision {
  bool can_thaw = false;
  bool must_refill_from_log = true;
  std::string reason;
};

// Returns can_thaw=true only when every identity, model, architecture, and
// dtype field matches and the deployment's KvDtypePolicy permits the
// manifest's kv_dtype. Otherwise can_thaw=false, must_refill_from_log=true,
// and reason names which check failed.
ThawDecision EvaluateCheckpointCompatibility(
    const proto::CheckpointAbi& manifest, const ThawRequest& request);

// Compares an expected manifest_hash against the manifest_hash actually
// computed during thaw. Mismatch returns must_refill_from_log=true. Both
// hashes are required; an empty hash on either side is treated as a
// mismatch since we cannot prove tamper-evidence without the digest.
ThawDecision EvaluateCheckpointThawVerification(const Hash256& expected,
                                                const Hash256& actual);

// Where the agent currently is on its trigger curve.
struct CheckpointTriggerState {
  int64_t tokens_since_checkpoint = 0;
  int64_t current_context_tokens = 0;
  int64_t max_context_tokens = 0;
  Event::Type last_event_type = Event::Type::kUser;
  bool explicit_handoff = false;
  bool last_event_was_milestone_tool = false;
  bool user_idle = false;
};

// Static configuration of the trigger.
struct CheckpointTriggerPolicy {
  int64_t min_delta_tokens = 2048;
  int64_t max_delta_tokens = 4096;
  // Fraction of the model's context window that triggers a checkpoint.
  // 0.75 matches the structure doc.
  double context_pressure_ratio = 0.75;
  bool checkpoint_on_handoff = true;
  bool checkpoint_on_correction = true;
  bool checkpoint_on_milestone_tool = true;
  bool checkpoint_on_idle = true;
  // Entropy-shift trigger is intentionally absent; it requires a secondary
  // classifier and breaks the SDK-free runtime stance.
};

struct CheckpointTriggerDecision {
  bool should_checkpoint = false;
  // Speculative checkpoints fire during user-read time (idle) and run
  // off the critical path; the orchestrator may skip them under
  // backpressure.
  bool speculative = false;
  proto::CheckpointTrigger trigger = proto::TRIGGER_UNSPECIFIED;
  std::string reason;
};

CheckpointTriggerDecision ShouldCreateCheckpoint(
    const CheckpointTriggerPolicy& policy,
    const CheckpointTriggerState& state);

// Inputs for the rack-local-vs-cross-AZ transport selector.
struct CheckpointTransportRequest {
  bool rack_local = false;
  bool rdma_available = false;
  bool cross_az_or_region = false;
};

// Selects RDMA/RoCE when both endpoints are rack-local and RDMA is
// available; otherwise falls back to gRPC + FlatBuffers.
proto::CheckpointTransportTier SelectCheckpointTransportTier(
    const CheckpointTransportRequest& request);

// Compaction decision: collapse a delta chain back to a fresh Level-0
// when the chain exceeds either the level cap or the cumulative byte cap.
// Defaults match the structure doc.
struct CheckpointCompactionPolicy {
  int max_delta_levels = 8;
  int64_t max_delta_bytes = 128 * 1024 * 1024;
};

bool ShouldCompactCheckpointDeltas(
    const std::vector<proto::CheckpointAbi>& delta_chain,
    const CheckpointCompactionPolicy& policy);

// Validates the storage-tier rules at the type level: blob ≠ MemoryDB,
// metadata = MemoryDB. Empty manifest_uri / blob_uri are rejected.
absl::Status ValidateCheckpointStorageTiers(
    const proto::CheckpointStorageBinding& binding);

}  // namespace litert::lm

#endif  // THIRD_PARTY_ODML_LITERT_LM_RUNTIME_DPM_CHECKPOINT_POLICY_H_
