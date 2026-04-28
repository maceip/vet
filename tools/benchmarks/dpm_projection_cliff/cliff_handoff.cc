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

#include "tools/benchmarks/dpm_projection_cliff/cliff_handoff.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/strings/match.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/dpm/checkpoint_policy.h"
#include "runtime/dpm/event.h"
#include "runtime/proto/checkpoint.pb.h"

namespace litert::lm::bench {
namespace {

// MITRE ATT&CK technique IDs we treat as kill-chain milestones. Hitting
// any of these in an event payload flips the milestone trigger because
// these are the "stage-boundary" tactics enterprise red-team playbooks
// pivot on.
constexpr std::array<absl::string_view, 13> kMilestoneTechniques = {
    "T1059",   // Command and Scripting Interpreter — execution start
    "T1003",   // OS Credential Dumping — credential phase
    "T1021",   // Remote Services — lateral movement
    "T1078",   // Valid Accounts — privilege escalation
    "T1486",   // Data Encrypted for Impact — ransomware impact
    "T1190",   // Exploit Public-Facing Application — initial access
    "T1133",   // External Remote Services — initial access
    "T1547",   // Boot or Logon Autostart Execution — persistence
    "T1098",   // Account Manipulation — persistence
    "T1071",   // Application Layer Protocol — c2 beacon
    "T1041",   // Exfiltration Over C2 Channel
    "T1083",   // File and Directory Discovery — recon
    "T1135",   // Network Share Discovery — recon
};

// Event type strings the corpus uses for explicit handoff events. The
// policy consumes a bool (`explicit_handoff`); the corpus author signals
// it by giving the event one of these type strings.
constexpr std::array<absl::string_view, 4> kHandoffTypes = {
    "handoff_request",
    "tier_escalation",
    "shift_change",
    "specialist_consult",
};

bool ContainsAnyOf(absl::string_view haystack,
                   const std::array<absl::string_view, 13>& needles) {
  for (const auto& n : needles) {
    if (absl::StrContains(haystack, n)) return true;
  }
  return false;
}

}  // namespace

std::string ClassifyEventType(absl::string_view yaml_type,
                              absl::string_view yaml_payload) {
  // Explicit handoff signals win first.
  for (const auto& h : kHandoffTypes) {
    if (yaml_type == h) return "handoff";
  }
  if (absl::StrContains(yaml_payload, "HANDOFF") ||
      absl::StrContains(yaml_payload, "handoff_to_tier") ||
      absl::StrContains(yaml_payload, "escalate_to")) {
    return "handoff";
  }

  // Correction events.
  if (yaml_type == "correction" || yaml_type == "analyst_correction" ||
      absl::StrContains(yaml_payload, "CORRECTION")) {
    return "correction";
  }

  // MITRE milestone in the payload → milestone tool.
  if (ContainsAnyOf(yaml_payload, kMilestoneTechniques)) {
    return "milestone";
  }

  // Bucket the rest by surface type. The corpus uses several:
  //   snoopy / auditd_event / syscall / process_event / network_event
  // → all map to "tool" (the agent observed an external signal).
  //   user_message / customer_message → "user"
  //   agent_response / model_response → "model"
  if (yaml_type == "user_message" || yaml_type == "customer_message" ||
      yaml_type == "ticket_create") {
    return "user";
  }
  if (yaml_type == "agent_response" || yaml_type == "model_response" ||
      yaml_type == "model") {
    return "model";
  }
  return "tool";
}

std::vector<CheckpointTraceEntry> SimulatePolicyDrivenCheckpoints(
    const std::vector<ClassifiedEvent>& events) {
  std::vector<CheckpointTraceEntry> trace;
  if (events.empty()) return trace;

  CheckpointTriggerPolicy policy;
  // Tighten the bench-specific knobs vs. the structure-doc defaults.
  // Red-team trajectories have ~50 events each averaging ~30-80 tokens;
  // a 2048-token min-delta would mean ~1 checkpoint per case which
  // defeats the "continuous audit trail" story. Drop to 256 tokens —
  // about every 4-8 events.
  policy.min_delta_tokens = 256;
  policy.max_delta_tokens = 1024;
  policy.checkpoint_on_handoff = true;
  policy.checkpoint_on_correction = true;
  policy.checkpoint_on_milestone_tool = true;
  policy.checkpoint_on_idle = false;  // No idle in batch trajectories.

  CheckpointTriggerState state;
  state.max_context_tokens = 32768;
  state.tokens_since_checkpoint = 0;
  state.current_context_tokens = 0;

  auto trigger_name = [](::litert::lm::proto::CheckpointTrigger t) {
    switch (t) {
      case ::litert::lm::proto::TRIGGER_HANDOFF:
        return std::string("TRIGGER_HANDOFF");
      case ::litert::lm::proto::TRIGGER_MILESTONE_TOOL:
        return std::string("TRIGGER_MILESTONE_TOOL");
      case ::litert::lm::proto::TRIGGER_TOKEN_THRESHOLD:
        return std::string("TRIGGER_TOKEN_THRESHOLD");
      case ::litert::lm::proto::TRIGGER_IDLE_SPECULATIVE:
        return std::string("TRIGGER_IDLE_SPECULATIVE");
      default:
        return std::string("TRIGGER_UNSPECIFIED");
    }
  };

  for (size_t i = 0; i < events.size(); ++i) {
    const auto& e = events[i];
    state.tokens_since_checkpoint += e.approx_tokens;
    state.current_context_tokens += e.approx_tokens;

    // Map the bucket to the bench-side trigger flags. The policy
    // expects a single Event::Type; we approximate.
    if (e.type_bucket == "handoff") {
      state.last_event_type = ::litert::lm::Event::Type::kCorrection;
      state.explicit_handoff = true;
      state.last_event_was_milestone_tool = false;
    } else if (e.type_bucket == "correction") {
      state.last_event_type = ::litert::lm::Event::Type::kCorrection;
      state.explicit_handoff = false;
      state.last_event_was_milestone_tool = false;
    } else if (e.type_bucket == "milestone") {
      state.last_event_type = ::litert::lm::Event::Type::kTool;
      state.explicit_handoff = false;
      state.last_event_was_milestone_tool = true;
    } else if (e.type_bucket == "user") {
      state.last_event_type = ::litert::lm::Event::Type::kUser;
      state.explicit_handoff = false;
      state.last_event_was_milestone_tool = false;
    } else if (e.type_bucket == "model") {
      state.last_event_type = ::litert::lm::Event::Type::kModel;
      state.explicit_handoff = false;
      state.last_event_was_milestone_tool = false;
    } else {
      state.last_event_type = ::litert::lm::Event::Type::kTool;
      state.explicit_handoff = false;
      state.last_event_was_milestone_tool = false;
    }

    auto decision = ::litert::lm::ShouldCreateCheckpoint(policy, state);
    if (decision.should_checkpoint) {
      CheckpointTraceEntry entry;
      entry.event_index = static_cast<int64_t>(i);
      entry.tokens_since_prev = state.tokens_since_checkpoint;
      entry.trigger = trigger_name(decision.trigger);
      entry.reason = decision.reason;
      entry.speculative = decision.speculative;
      trace.push_back(std::move(entry));
      state.tokens_since_checkpoint = 0;
    }
  }
  return trace;
}

int64_t FindHandoffIndex(const std::vector<ClassifiedEvent>& events) {
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].type_bucket == "handoff") return static_cast<int64_t>(i);
  }
  return -1;
}

namespace {
// Severe-milestone MITRE techniques: the canonical Tier-1 → Tier-2
// escalation points. Subset of kMilestoneTechniques restricted to the
// stages a real SOC playbook escalates on (credential access, lateral,
// privilege via valid accounts, impact). T1083/T1135 (discovery) are
// deliberately excluded — they fire on noise.
constexpr std::array<absl::string_view, 4> kSevereMilestoneTechniques = {
    "T1003",   // OS Credential Dumping
    "T1021",   // Remote Services
    "T1078",   // Valid Accounts
    "T1486",   // Data Encrypted for Impact
};
}  // namespace

int64_t FindSyntheticHandoffIndex(
    const std::vector<ClassifiedEvent>& events) {
  if (events.empty()) return -1;
  // Prefer the first severe milestone.
  for (size_t i = 0; i < events.size(); ++i) {
    for (const auto& t : kSevereMilestoneTechniques) {
      if (absl::StrContains(events[i].text, t)) {
        return static_cast<int64_t>(i);
      }
    }
  }
  // Fall back to first milestone of any kind.
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].type_bucket == "milestone") {
      return static_cast<int64_t>(i);
    }
  }
  // Last resort: median position.
  return static_cast<int64_t>(events.size() / 2);
}

HandoffIndexResolution ResolveHandoffIndex(
    const std::vector<ClassifiedEvent>& events) {
  HandoffIndexResolution out;
  if (events.empty()) {
    out.kind = "none";
    return out;
  }
  const int64_t explicit_idx = FindHandoffIndex(events);
  if (explicit_idx >= 0) {
    out.index = explicit_idx;
    out.kind = "explicit";
    return out;
  }
  // Try severe milestone first.
  for (size_t i = 0; i < events.size(); ++i) {
    for (const auto& t : kSevereMilestoneTechniques) {
      if (absl::StrContains(events[i].text, t)) {
        out.index = static_cast<int64_t>(i);
        out.kind = "synthetic_severe_milestone";
        return out;
      }
    }
  }
  // Fall back to first milestone.
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].type_bucket == "milestone") {
      out.index = static_cast<int64_t>(i);
      out.kind = "synthetic_milestone";
      return out;
    }
  }
  // Median.
  out.index = static_cast<int64_t>(events.size() / 2);
  out.kind = "synthetic_median";
  return out;
}

}  // namespace litert::lm::bench
