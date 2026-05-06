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

#ifndef THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_CLIFF_HANDOFF_H_
#define THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_CLIFF_HANDOFF_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"  // from @com_google_absl

namespace litert::lm::bench {

// One event from a corpus trajectory, classified by the harness so the
// checkpoint policy can decide. The corpus YAML stores arbitrary type
// strings ("snoopy", "auditd_event", "handoff_request", ...); this
// struct is the strongly-typed projection the harness operates on.
struct ClassifiedEvent {
  // Verbatim event text from the corpus, what the model sees.
  std::string text;
  // Approximate token count (chars / 4 if no tokenizer plumbed). Drives
  // CheckpointTriggerState.tokens_since_checkpoint.
  int approx_tokens = 0;
  // Type bucket for the policy:
  //   "user", "model", "tool", "correction", "handoff", "milestone"
  // "handoff" means the event itself signals "handoff to next analyst";
  // "milestone" means a kill-chain-stage transition the policy treats as
  // a milestone-tool boundary.
  std::string type_bucket;
};

// Per-checkpoint trace row. The harness emits one of these into the
// CliffRow's checkpoint_log when the policy fires a checkpoint while a
// trajectory is being walked.
struct CheckpointTraceEntry {
  int64_t event_index = 0;        // 0-based position in the trajectory
  int64_t tokens_since_prev = 0;
  std::string trigger;            // "TRIGGER_HANDOFF" / "TRIGGER_TOKEN_THRESHOLD" / ...
  std::string reason;              // human-friendly reason from policy
  bool speculative = false;
  // Substrate cost for THIS checkpoint emission, if it actually wrote.
  // Empty (0) means the trace was logged but no PUT fired (e.g. the
  // bench is in dry-run / skip mode for this trace step).
  uint64_t bytes_uploaded = 0;
  double wall_put_ms = 0.0;
  std::string body_hash_hex;       // BLAKE3 of the projected memory at this point
};

// Boundary-test outcome for one cell. The bench actively tries each
// failure mode and records whether it was correctly rejected.
struct HandoffBoundaryReport {
  bool cross_tenant_breach_blocked = false;
  bool expired_credential_blocked = false;
  bool tampered_audit_detected = false;
  bool replay_blocked = false;
  std::string notes;               // short text per failed assertion
};

// Per-cell handoff result. Populated when a dpm_checkpoints_handoff
// cell runs; serialized into the JSONL row alongside the existing
// substrate fields.
struct HandoffCellResult {
  std::string handoff_id;          // UUID v7
  std::string from_agent_role;     // "analyst.tier1"
  std::string to_agent_role;       // "analyst.tier2"
  int64_t handoff_event_index = 0;
  int64_t total_events = 0;
  std::vector<CheckpointTraceEntry> checkpoint_log;
  // Cumulative substrate cost across all checkpoints in this cell.
  uint64_t cumulative_bytes_uploaded = 0;
  uint64_t cumulative_bytes_downloaded = 0;
  double cumulative_wall_put_ms = 0.0;
  // Resume cost on B's side: GET round-trip + Conversation init.
  double wall_to_resume_ms = 0.0;
  // Decision agreement: "agree" | "refine" | "overrule" | "re_escalate".
  // Computed by string-matching B's decision text against A's
  // decision_label and the case ground_truth. "" when unevaluable.
  std::string b_action;
  // Cold-baseline companion (the same case but with B forced to read
  // every event from the raw log instead of from a checkpoint). Empty
  // when not run.
  uint64_t cold_baseline_bytes_fetched = 0;
  double cold_baseline_wall_ms = 0.0;
  HandoffBoundaryReport boundary;
};

// Classifies a corpus event's type-string into the policy bucket. Pure
// function, no I/O. Knows the corpus-specific vocabulary
// (snoopy/auditd_event/syscall/handoff_request/...) and the MITRE
// kill-chain milestone keywords (T1059, T1003, T1021, T1486, ...).
std::string ClassifyEventType(absl::string_view yaml_type,
                              absl::string_view yaml_payload);

// Walks the trajectory and returns the trace of policy decisions. Does
// not actually persist anything to S3 — that's the harness's job after
// it sees the trace. This separation lets the harness use the trace
// to decide which checkpoints to actually PUT (e.g. it may sample, or
// skip speculative checkpoints under backpressure).
std::vector<CheckpointTraceEntry> SimulatePolicyDrivenCheckpoints(
    const std::vector<ClassifiedEvent>& events);

// First event index where ClassifyEventType returned "handoff". -1 if
// no explicit handoff event in the trajectory.
int64_t FindHandoffIndex(const std::vector<ClassifiedEvent>& events);

// Synthesises a handoff index when the corpus didn't carry one. The
// curated red-team trajectories (CasinoLimit / pwnjutsu) don't have
// explicit handoff_request events but they do have natural Tier-1 →
// Tier-2 escalation points: the first occurrence of a "severe"
// milestone technique — credential access (T1003), lateral movement
// (T1021), valid-account abuse (T1078), or impact (T1486). These are
// where a real SOC analyst would page the next tier.
//
// Order of preference:
//   1. First event whose payload contains T1003 / T1021 / T1078 / T1486.
//   2. First "milestone"-bucketed event of any kind.
//   3. Median event index (so the test still has a position to use).
// Returns -1 only for empty trajectories.
int64_t FindSyntheticHandoffIndex(const std::vector<ClassifiedEvent>& events);

// Resolves the effective handoff index used by the bench. Returns
// {index, kind} where kind is one of:
//   "explicit"                  — corpus has a handoff_* event
//   "synthetic_severe_milestone" — fell through to severe MITRE technique
//   "synthetic_milestone"        — fell through to any milestone
//   "synthetic_median"           — fell through to median position
//   "none"                       — empty trajectory
struct HandoffIndexResolution {
  int64_t index = -1;
  std::string kind;
};
HandoffIndexResolution ResolveHandoffIndex(
    const std::vector<ClassifiedEvent>& events);

}  // namespace litert::lm::bench

#endif  // THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_CLIFF_HANDOFF_H_
