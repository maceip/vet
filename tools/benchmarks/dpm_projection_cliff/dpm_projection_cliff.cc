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

#include "tools/benchmarks/dpm_projection_cliff/dpm_projection_cliff.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/numbers.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/str_split.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "absl/strings/strip.h"  // from @com_google_absl

namespace litert::lm::bench {
namespace {

// Locale-independent double formatting. printf with %g would honor the
// locale's decimal separator on some platforms; we want a stable JSON
// output so the JSONL is byte-stable across rigs.
std::string FormatDouble(double v) {
  std::ostringstream oss;
  oss.imbue(std::locale::classic());
  oss << v;
  return oss.str();
}

void AppendQuoted(absl::string_view s, std::string* out) {
  out->push_back('"');
  for (char c : s) {
    switch (c) {
      case '\\': out->append("\\\\"); break;
      case '"':  out->append("\\\""); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          absl::StrAppend(out, "\\u",
                          absl::Hex(static_cast<unsigned char>(c),
                                    absl::kZeroPad4));
        } else {
          out->push_back(c);
        }
    }
  }
  out->push_back('"');
}

void AppendKey(absl::string_view key, std::string* out, bool first) {
  if (!first) out->push_back(',');
  AppendQuoted(key, out);
  out->push_back(':');
}

void AppendStr(absl::string_view key, absl::string_view value,
               std::string* out, bool* first) {
  AppendKey(key, out, *first);
  AppendQuoted(value, out);
  *first = false;
}

void AppendInt(absl::string_view key, int64_t value, std::string* out,
               bool* first) {
  AppendKey(key, out, *first);
  absl::StrAppend(out, value);
  *first = false;
}

void AppendUint(absl::string_view key, uint64_t value, std::string* out,
                bool* first) {
  AppendKey(key, out, *first);
  absl::StrAppend(out, value);
  *first = false;
}

void AppendDouble(absl::string_view key, double value, std::string* out,
                  bool* first) {
  AppendKey(key, out, *first);
  out->append(FormatDouble(value));
  *first = false;
}

void AppendBool(absl::string_view key, bool value, std::string* out,
                bool* first) {
  AppendKey(key, out, *first);
  out->append(value ? "true" : "false");
  *first = false;
}

}  // namespace

std::string CliffRowToJsonl(const CliffRow& row) {
  std::string out;
  out.reserve(512);
  out.push_back('{');
  bool first = true;
  AppendInt("schema_version", row.schema_version, &out, &first);
  AppendStr("condition", row.condition, &out, &first);
  AppendUint("trajectory_chars", row.trajectory_chars, &out, &first);
  AppendUint("memory_budget_chars", row.memory_budget_chars, &out, &first);
  AppendUint("repeat_idx", row.repeat_idx, &out, &first);
  AppendUint("seed", row.seed, &out, &first);
  AppendStr("architecture_tag", row.architecture_tag, &out, &first);
  AppendStr("manifest_hash", row.manifest_hash, &out, &first);
  AppendStr("runtime_version", row.runtime_version, &out, &first);
  AppendStr("kv_dtype", row.kv_dtype, &out, &first);
  AppendBool("kv_dtype_policy_replay_safe",
             row.kv_dtype_policy_replay_safe, &out, &first);
  AppendStr("model_class", row.model_class, &out, &first);

  AppendDouble("decision_score", row.decision_score, &out, &first);
  AppendDouble("wall_clock_decision_ms", row.wall_clock_decision_ms, &out,
               &first);
  AppendDouble("wall_clock_append_p50_us", row.wall_clock_append_p50_us,
               &out, &first);
  AppendDouble("wall_clock_append_p99_us", row.wall_clock_append_p99_us,
               &out, &first);
  if (row.wall_clock_checkpoint_put_ms.has_value()) {
    AppendDouble("wall_clock_checkpoint_put_ms",
                 *row.wall_clock_checkpoint_put_ms, &out, &first);
  }
  if (row.wall_clock_thaw_ms.has_value()) {
    AppendDouble("wall_clock_thaw_ms", *row.wall_clock_thaw_ms, &out,
                 &first);
  }
  if (row.kv_bytes_per_1024_tokens.has_value()) {
    AppendInt("kv_bytes_per_1024_tokens", *row.kv_bytes_per_1024_tokens,
              &out, &first);
  }
  if (row.must_refill_from_log.has_value()) {
    AppendBool("must_refill_from_log", *row.must_refill_from_log, &out,
               &first);
  }

  AppendUint("disk_bytes_session_total", row.disk_bytes_session_total,
             &out, &first);
  AppendUint("disk_bytes_event_log", row.disk_bytes_event_log, &out,
             &first);
  AppendUint("disk_bytes_checkpoint_blobs", row.disk_bytes_checkpoint_blobs,
             &out, &first);

  if (!row.tamper_test_json.empty()) {
    AppendKey("tamper_test", &out, first);
    out.append(row.tamper_test_json);
    first = false;
  }
  if (row.mock) {
    AppendBool("mock", true, &out, &first);
  }
  out.push_back('}');
  return out;
}

namespace {

absl::Status ParseScalar(absl::string_view raw, std::string* out) {
  raw = absl::StripAsciiWhitespace(raw);
  if (!raw.empty() && (raw.front() == '"' || raw.front() == '\'')) {
    if (raw.size() < 2 || raw.back() != raw.front()) {
      return absl::InvalidArgumentError("unterminated quoted scalar");
    }
    *out = std::string(raw.substr(1, raw.size() - 2));
    return absl::OkStatus();
  }
  *out = std::string(raw);
  return absl::OkStatus();
}

absl::Status ParseUint(absl::string_view raw, uint64_t* out) {
  return absl::SimpleAtoi(absl::StripAsciiWhitespace(raw), out)
             ? absl::OkStatus()
             : absl::InvalidArgumentError(absl::StrCat(
                   "expected non-negative integer, got '", raw, "'"));
}

absl::Status ParseBool(absl::string_view raw, bool* out) {
  std::string lower(absl::StripAsciiWhitespace(raw));
  for (auto& c : lower) c = static_cast<char>(std::tolower(c));
  if (lower == "true" || lower == "yes" || lower == "1") {
    *out = true;
    return absl::OkStatus();
  }
  if (lower == "false" || lower == "no" || lower == "0") {
    *out = false;
    return absl::OkStatus();
  }
  return absl::InvalidArgumentError(
      absl::StrCat("expected boolean, got '", raw, "'"));
}

}  // namespace

absl::StatusOr<CliffConfig> LoadConfig(absl::string_view yaml_path) {
  std::ifstream in{std::string(yaml_path)};
  if (!in.is_open()) {
    return absl::NotFoundError(
        absl::StrCat("LoadConfig: cannot open ", yaml_path));
  }
  CliffConfig cfg;
  std::string line;
  std::string current_list_key;
  while (std::getline(in, line)) {
    // Strip comments.
    const auto hash_pos = line.find('#');
    if (hash_pos != std::string::npos) {
      line.erase(hash_pos);
    }
    // Skip blank lines.
    auto stripped = absl::StripAsciiWhitespace(line);
    if (stripped.empty()) continue;

    // List item.
    if (stripped.size() >= 2 && stripped.front() == '-' &&
        stripped[1] == ' ') {
      if (current_list_key.empty()) {
        return absl::InvalidArgumentError(
            "list item without an active key in YAML config.");
      }
      const auto value = absl::StripAsciiWhitespace(stripped.substr(2));
      if (current_list_key == "conditions") {
        std::string s;
        auto status = ParseScalar(value, &s);
        if (!status.ok()) return status;
        cfg.conditions.push_back(s);
      } else if (current_list_key == "trajectory_chars") {
        uint64_t v;
        auto status = ParseUint(value, &v);
        if (!status.ok()) return status;
        cfg.trajectory_chars.push_back(v);
      } else if (current_list_key == "memory_budget_chars") {
        uint64_t v;
        auto status = ParseUint(value, &v);
        if (!status.ok()) return status;
        cfg.memory_budget_chars.push_back(v);
      } else if (current_list_key == "scenarios") {
        std::string s;
        auto status = ParseScalar(value, &s);
        if (!status.ok()) return status;
        cfg.scenarios.push_back(s);
      } else {
        // Unknown list keys are skipped to keep this parser tolerant of
        // additions in newer config files; the chart pipeline only
        // depends on the keys above.
      }
      continue;
    }
    current_list_key.clear();

    // Top-level key: value.
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key(absl::StripAsciiWhitespace(
        absl::string_view(line.data(), colon)));
    const std::string value(absl::StripAsciiWhitespace(
        absl::string_view(line.data() + colon + 1,
                          line.size() - colon - 1)));
    if (value.empty()) {
      // Key with a list following it.
      current_list_key = key;
      continue;
    }
    if (key == "name") {
      auto status = ParseScalar(value, &cfg.name);
      if (!status.ok()) return status;
    } else if (key == "seed") {
      auto status = ParseUint(value, &cfg.seed);
      if (!status.ok()) return status;
    } else if (key == "repeats") {
      uint64_t v = 0;
      auto status = ParseUint(value, &v);
      if (!status.ok()) return status;
      cfg.repeats = static_cast<uint32_t>(v);
    } else if (key == "model_class") {
      auto status = ParseScalar(value, &cfg.model_class);
      if (!status.ok()) return status;
    } else if (key == "kv_dtype") {
      auto status = ParseScalar(value, &cfg.kv_dtype);
      if (!status.ok()) return status;
    } else if (key == "kv_dtype_policy_replay_safe") {
      auto status = ParseBool(value, &cfg.kv_dtype_policy_replay_safe);
      if (!status.ok()) return status;
    } else if (key == "record_per_call_timers") {
      auto status = ParseBool(value, &cfg.record_per_call_timers);
      if (!status.ok()) return status;
    } else {
      // Unknown scalar keys are tolerated so newer configs can add
      // metadata without breaking older drivers; the runtime only
      // honors the keys above.
    }
  }
  return cfg;
}

namespace {

// Stubbed measurement for the scaffolding commit. Returns plausible
// shapes tagged mock=true so the chart pipeline can be reviewed end-to-
// end before the real wiring against runtime/dpm + runtime/platform.
// The real RunOneCell will replace this with actual timers around
// EventSourcedLog::Append, DPMProjector::Project,
// CheckpointStore::Put, and the cross-architecture thaw paths.
CliffRow MockRow(const CliffConfig& config, absl::string_view condition,
                 uint64_t trajectory_chars, uint64_t memory_budget_chars,
                 uint32_t repeat_idx) {
  std::seed_seq seq{config.seed,
                    static_cast<uint64_t>(repeat_idx),
                    trajectory_chars, memory_budget_chars};
  std::mt19937_64 rng(seq);
  std::uniform_real_distribution<double> jitter(-0.01, 0.01);

  CliffRow row;
  row.condition = std::string(condition);
  row.trajectory_chars = trajectory_chars;
  row.memory_budget_chars = memory_budget_chars;
  row.repeat_idx = repeat_idx;
  row.seed = config.seed;
  row.architecture_tag = "mock-rig-x86_64";
  row.manifest_hash = std::string(64, 'a');
  row.runtime_version = "litertlm-bench-mock";
  row.kv_dtype = config.kv_dtype;
  row.kv_dtype_policy_replay_safe = config.kv_dtype_policy_replay_safe;
  row.model_class = config.model_class;

  // Decision-alignment cliff curves (matched to plot.py's mock).
  double base = 0.93;
  if (condition == "summarization_baseline") {
    base = std::max(0.05, 0.95 -
                              0.85 * std::max<double>(
                                         0, (double)trajectory_chars - 5000) /
                                  95000.0);
  } else if (condition == "dpm_projection") {
    base = 0.93 - 0.04 * std::max<double>(
                            0, (double)trajectory_chars - 100000) /
                            100000.0;
  } else if (condition == "dpm_checkpoints") {
    base = 0.93 - 0.03 * std::max<double>(
                            0, (double)trajectory_chars - 100000) /
                            100000.0;
  } else if (condition == "dpm_checkpoints_prefix_cached") {
    base = 0.93 - 0.02 * std::max<double>(
                            0, (double)trajectory_chars - 100000) /
                            100000.0;
  }
  row.decision_score = std::max(0.0, std::min(1.0, base + jitter(rng)));

  if (condition == "summarization_baseline") {
    row.wall_clock_decision_ms = 200 + 0.06 * trajectory_chars;
  } else if (condition == "dpm_projection") {
    row.wall_clock_decision_ms = 60 + 0.012 * trajectory_chars;
  } else if (condition == "dpm_checkpoints") {
    row.wall_clock_decision_ms = 30 + 0.004 * trajectory_chars;
    row.wall_clock_checkpoint_put_ms = 120.0 + jitter(rng) * 1000.0;
  } else if (condition == "dpm_checkpoints_prefix_cached") {
    row.wall_clock_decision_ms = 18 + 0.002 * trajectory_chars;
    row.wall_clock_checkpoint_put_ms = 120.0 + jitter(rng) * 1000.0;
    row.wall_clock_thaw_ms = 22.0 + jitter(rng) * 100.0;
  }

  row.wall_clock_append_p50_us = 50.0 + jitter(rng) * 100.0;
  row.wall_clock_append_p99_us = 1000.0 + jitter(rng) * 5000.0;
  row.disk_bytes_event_log = 4 * trajectory_chars;
  row.disk_bytes_checkpoint_blobs =
      (condition == "dpm_checkpoints" ||
       condition == "dpm_checkpoints_prefix_cached")
          ? 20 * 1024 * 1024
          : 0;
  row.disk_bytes_session_total =
      row.disk_bytes_event_log + row.disk_bytes_checkpoint_blobs;
  row.mock = true;
  return row;
}

}  // namespace

absl::StatusOr<CliffRow> RunOneCell(const CliffConfig& config,
                                    absl::string_view condition,
                                    uint64_t trajectory_chars,
                                    uint64_t memory_budget_chars,
                                    uint32_t repeat_idx) {
  // TODO(phase2-bench): replace the mock with real measurement against
  // EventSourcedLog::Append, DPMProjector::Project,
  // CheckpointStore::Put, MerkleDagStore::Put, and the thaw path. Until
  // then this returns a mock=true row so the JSONL pipeline + plot.py
  // can be exercised end-to-end.
  return MockRow(config, condition, trajectory_chars, memory_budget_chars,
                 repeat_idx);
}

}  // namespace litert::lm::bench
