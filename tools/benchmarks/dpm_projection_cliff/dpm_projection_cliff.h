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

#ifndef THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_DPM_PROJECTION_CLIFF_H_
#define THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_DPM_PROJECTION_CLIFF_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl

namespace litert::lm::bench {

// One row of the JSONL output. Mirrors schema.md verbatim. Keep this in
// sync with plot.py's Row dataclass; schema_version bumps must be done in
// both files at once.
struct CliffRow {
  int schema_version = 1;

  std::string condition;
  uint64_t trajectory_chars = 0;
  uint64_t memory_budget_chars = 0;
  uint32_t repeat_idx = 0;
  uint64_t seed = 20260420;
  std::string architecture_tag;
  std::string manifest_hash;     // hex BLAKE3 of the loaded model bundle
  std::string runtime_version;
  std::string kv_dtype;
  bool kv_dtype_policy_replay_safe = true;
  std::string model_class;

  double decision_score = 0.0;
  double wall_clock_decision_ms = 0.0;
  double wall_clock_append_p50_us = 0.0;
  double wall_clock_append_p99_us = 0.0;

  std::optional<double> wall_clock_checkpoint_put_ms;
  std::optional<double> wall_clock_thaw_ms;
  std::optional<int64_t> kv_bytes_per_1024_tokens;
  std::optional<bool> must_refill_from_log;

  uint64_t disk_bytes_session_total = 0;
  uint64_t disk_bytes_event_log = 0;
  uint64_t disk_bytes_checkpoint_blobs = 0;

  // Adversarial-audit rows only. JSON-encoded inline; the driver builds
  // it as a small std::string of pre-escaped JSON because we don't pull
  // a JSON library at the bench layer.
  std::string tamper_test_json;

  // Set to true when the row is synthesized by --mock or by a stub
  // condition handler. Production runs against real hardware leave
  // this false; the writer adds the field only when true.
  bool mock = false;
};

// Serializes one CliffRow to a single JSONL line (no trailing newline).
// Numeric formatting is locale-independent; optional fields are omitted
// rather than emitted as null so the schema stays compact.
std::string CliffRowToJsonl(const CliffRow& row);

// Loaded benchmark configuration. Mirrors the YAML schemas under
// configs/. The driver does not link a YAML library; instead the
// scaffolding shipped here parses the small subset of YAML we use
// (top-level scalars and lists of scalars) by hand. Production
// integrations can swap in a real YAML loader if we ever need nested
// structures.
struct CliffConfig {
  std::string name;
  uint64_t seed = 20260420;
  uint32_t repeats = 1;
  std::vector<std::string> conditions;
  std::vector<uint64_t> trajectory_chars;
  std::vector<uint64_t> memory_budget_chars;
  std::string model_class = "gqa";
  std::string kv_dtype = "fp16";
  bool kv_dtype_policy_replay_safe = true;
  bool record_per_call_timers = false;
  // Adversarial-audit scenarios; empty for the non-adversarial configs.
  std::vector<std::string> scenarios;
};

absl::StatusOr<CliffConfig> LoadConfig(absl::string_view yaml_path);

// Drives one (condition, trajectory, budget, repeat) cell against the
// merged DPM substrate and returns the row. Stubbed in this scaffolding
// commit: the function fills in plausible numbers tagged mock=true so the
// chart pipeline compiles end-to-end. Real measurement lands in a follow-
// up that wires this against runtime/dpm and runtime/platform/.
absl::StatusOr<CliffRow> RunOneCell(const CliffConfig& config,
                                    absl::string_view condition,
                                    uint64_t trajectory_chars,
                                    uint64_t memory_budget_chars,
                                    uint32_t repeat_idx);

}  // namespace litert::lm::bench

#endif  // THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_DPM_PROJECTION_CLIFF_H_
