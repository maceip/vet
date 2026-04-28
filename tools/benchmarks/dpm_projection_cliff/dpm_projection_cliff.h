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

  // Configuration.
  std::string condition;
  uint64_t trajectory_chars = 0;
  uint64_t memory_budget_chars = 0;
  uint32_t repeat_idx = 0;
  uint64_t seed = 20260420;
  std::string kv_dtype;
  bool kv_dtype_policy_replay_safe = true;
  std::string model_class;

  // Decision-alignment axes. Per peer review: the paper's result is
  // axis-specific, so the per-axis fields are primary. decision_score
  // is optional/derived.
  std::optional<double> frp;
  std::optional<double> rcs;
  std::optional<double> eda;
  std::optional<double> crr;
  std::optional<double> decision_score;
  // Mean over only locally scored deterministic axes. In corpus mode,
  // RCS is commonly judge-only; this field lets charts show immediate
  // FRP/EDA/CRR signal without pretending the judge axis failed.
  std::optional<double> deterministic_score;
  // Number of axes actually scored by this producer. A complete paper-
  // comparable row has 4; pre-judge corpus rows commonly have 3.
  std::optional<int64_t> scored_axis_count;
  // Comma-separated axes waiting for an external judge, e.g. "rcs".
  std::string pending_judge_axes;

  // Wall-clock timers.
  double wall_clock_decision_ms = 0.0;
  double wall_clock_append_p50_us = 0.0;
  double wall_clock_append_p99_us = 0.0;
  std::optional<double> wall_clock_checkpoint_put_ms;
  std::optional<double> wall_clock_thaw_ms;
  // Total wall-clock spent before the final decision call. For DPM this
  // is projection time; for rolling_summary_baseline this is N summary
  // updates. Checkpoint put/thaw timers remain storage-specific.
  std::optional<double> wall_clock_memory_build_ms;

  // Disk and KV size.
  uint64_t disk_bytes_session_total = 0;
  uint64_t disk_bytes_event_log = 0;
  uint64_t disk_bytes_checkpoint_blobs = 0;
  std::optional<int64_t> kv_bytes_per_1024_tokens;
  std::optional<bool> must_refill_from_log;
  // Reader-facing lane labels. They keep charts honest:
  //   quality: paper baseline vs DPM decision quality
  //   ops: checkpointing latency/bytes/provenance
  //   audit: handoff and boundary tests
  std::string evidence_lane;
  std::string claim_tested;

  // Provenance: makes the row replay-traceable. Production runs
  // populate every field; absence is tolerated for older JSONL but the
  // audit story leans on these being present.
  std::string architecture_tag;
  std::string manifest_hash;          // BLAKE3 of canonical CheckpointAbi
  std::string model_artifact_hash;    // BLAKE3 of model bundle
  std::string runtime_version;
  std::string config_hash;            // BLAKE3 of the YAML config
  std::string git_sha;                // 40-char build SHA
  std::optional<bool> dirty_tree;
  std::string hostname;
  std::string os;
  std::string cpu_model;
  std::string accelerator_id;

  // Adversarial-audit rows only. Pre-escaped JSON object.
  std::string tamper_test_json;

  // Corpus identification (set when the cell came from a hand-curated
  // YAML case rather than the synthetic generator). Empty for synthetic
  // rows so the JSONL stays byte-stable across both modes.
  std::string case_id;
  std::string domain;
  std::string decision_label_expected;  // ground_truth.decision_label
  std::string decision_label_observed;  // first token of decision response

  // Smoke-debug fields. Populated for real-substrate rows so the
  // operator can tell whether a low score reflects matcher strictness,
  // a clipped projection, missing citations, or actual model failure.
  // Each is omitted from the JSONL when its sentinel is set (-1 / "" /
  // nullopt) so older rows stay byte-stable.
  int64_t projection_chars = -1;
  int64_t projection_token_cap = -1;
  std::optional<bool> projection_truncated;
  std::optional<double> citation_coverage;     // probes hit / probes asked
  std::string raw_projection_path;             // sidecar text file
  std::string raw_decision_path;               // sidecar text file
  std::string scoring_misses;                  // CSV of axis names

  // Network / checkpoint-substrate fields. Populated when a
  // dpm_checkpoints* condition runs against an S3-backed
  // CheckpointStore. The local-filesystem store leaves these unset.
  // Counters are deltas across the cell, not cumulative across the run.
  std::optional<uint64_t> network_bytes_uploaded;
  std::optional<uint64_t> network_bytes_downloaded;
  std::optional<int64_t> checkpoint_count;        // # PutPayload calls
  std::optional<double> checkpoint_put_p50_ms;
  std::optional<double> checkpoint_put_p99_ms;
  std::string checkpoint_backend;                  // "local_fs" | "s3_express"
  std::string checkpoint_endpoint;                 // S3 host (when s3_express)

  // ----- Agent-to-agent handoff (dpm_checkpoints_handoff condition) -----
  // Populated only when the row came from the handoff condition. Empty
  // / nullopt for all other conditions so the JSONL stays byte-stable.
  std::string handoff_id;             // UUID v7 from the broker
  std::string handoff_from_role;      // "analyst.tier1"
  std::string handoff_to_role;        // "analyst.tier2"
  std::string handoff_intent_kind;    // "tier_escalation" | ...
  // Whether the handoff index came from an explicit corpus event or
  // was synthesized by the harness. Values:
  //   "explicit", "synthetic_severe_milestone",
  //   "synthetic_milestone", "synthetic_median".
  std::string handoff_kind;
  std::optional<int64_t> handoff_event_index;
  std::optional<int64_t> handoff_total_events;
  std::optional<int64_t> handoff_checkpoint_count;
  std::optional<uint64_t> handoff_cumulative_bytes_uploaded;
  std::optional<uint64_t> handoff_cumulative_bytes_downloaded;
  std::optional<double> handoff_cumulative_wall_put_ms;
  std::optional<double> handoff_wall_to_resume_ms;
  std::string handoff_b_action;       // "agree" | "refine" | "overrule" | "re_escalate"
  std::optional<uint64_t> handoff_cold_baseline_bytes_fetched;
  std::optional<double> handoff_cold_baseline_wall_ms;
  // Boundary tests as code, recorded per row so a regression in any
  // safety property is visible in the chart deck.
  std::optional<bool> cross_tenant_breach_blocked;
  std::optional<bool> expired_credential_blocked;
  std::optional<bool> tampered_audit_detected;
  std::optional<bool> replay_blocked;
  // Pre-escaped JSON object summarising the per-checkpoint trace
  // (event_index, trigger, reason, bytes, wall_ms, body_hash).
  std::string handoff_checkpoint_trace_json;

  // Set to true when the row is synthesized by --mock or by a stub
  // handler. Production runs leave it false; the JSONL writer emits
  // the field only when true.
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
  std::vector<std::string> scenarios;
  // When true, RunOneCell returns mock=true rows. Off by default; the
  // driver flips it on only when --allow_mock is passed at the binary
  // level. Production runs against real hardware leave this false so
  // a stale config cannot accidentally produce mock rows.
  bool allow_mock = false;

  // Real-substrate fields. Populated by the main binary from CLI flags
  // (not from YAML), which is why they live next to allow_mock here
  // rather than in the YAML loader. Empty model_path forces the mock
  // path even when allow_mock is true; the driver fails closed when
  // both model_path is empty and allow_mock is false.
  std::string model_path;
  std::string backend = "cpu";  // cpu | gpu | npu
  std::string checkpoint_root;  // base dir for CheckpointStore.
  int max_num_tokens = 32768;   // KV cache capacity.
  // Empirically validated prompt-token ceiling. This is intentionally
  // lower than max_num_tokens: the Gemma4 CPU decoder can segfault after
  // successful prefill at long context, so the bench refuses unvalidated
  // prompts and emits a skipped row instead of calling decode. <=0
  // disables the safety gate for explicit repro/debug runs.
  int safe_max_tokens = 12000;
  // CPU dynamic-prefill chunk size. -1 keeps the model default; set
  // explicitly (e.g. 1024) to bound each prefill iteration on CPU and
  // dodge the >32k-char Gemma-4 segfault. Passed through to
  // CpuConfig.prefill_chunk_size in BuildEngineSettings.
  int prefill_chunk_size = -1;
  // CPU thread count override. 0 keeps the model default.
  int num_cpu_threads = 0;
  // When true, force a serialized WebGPU upload/compile path for debugging
  // unstable Windows adapters. Production GPU runs keep LiteRT-LM's model
  // defaults so the benchmark does not accidentally measure a nonstandard
  // accelerator configuration.
  bool conservative_gpu_settings = false;
  // Stage-1 (projection) decode budget. -1 means "derive from
  // memory_budget_chars / 3 + 128, clamped to max_num_tokens" — the
  // projection has to fit roughly memory_budget_chars of text, so
  // letting stage 1 run with the (tiny) decision budget would clip it.
  int projection_max_output_tokens = -1;
  // Stage-2 (decision) decode budget — just needs to fit the four
  // probe answers. Defaults tight; cells that consistently overflow
  // will manifest as truncated answers in the scorer.
  int decision_max_output_tokens = 256;
  // Legacy compat alias — when set, overrides decision_max_output_tokens.
  int max_output_tokens = -1;
  std::string schema_id = "insurance_liability_v2";
  std::string schema_json =
      R"json({"Facts":["string with one-based [i] citation"],)json"
      R"json("Reasoning":["string with one-based [i] citation"],)json"
      R"json("Compliance":["string with one-based [i] citation"]})json";
  std::string config_path;  // For provenance hashing only.

  // Checkpoint backend selection. "local_fs" (default) writes to
  // checkpoint_root on the local filesystem; "s3_express" writes to a
  // single-AZ S3 Express bucket whose host is derived from
  // {s3_bucket, s3_az_id, region}. The s3_* fields are required when
  // backend == "s3_express".
  std::string checkpoint_backend = "local_fs";
  std::string s3_bucket;
  std::string s3_az_id;       // e.g. "euc1-az1" for eu-central-1a
  std::string s3_region;      // e.g. "eu-central-1"

  // Corpus mode: when corpus_dir is non-empty, the driver iterates
  // hand-curated YAML cases under it instead of synthesizing
  // trajectories. trajectory_chars / memory_budget_chars / repeats /
  // conditions still parameterize the sweep; the corpus replaces only
  // the trajectory + ground-truth source.
  std::string corpus_dir;
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

// Forward decl — full def in tools/benchmarks/dpm_projection_cliff/
// cliff_corpus.h. We avoid including the header here so callers that
// don't touch the corpus path don't pay the parse cost.
struct CliffCorpusCase;

// Corpus-mode entry point. Same shape as RunOneCell but draws the
// trajectory + ground truth from a pre-loaded case instead of the
// synthetic generator. The case's trajectory_chars (i.e. event_log
// length in chars) is recorded into the row's trajectory_chars field
// regardless of any value the operator passed; the memory budget is
// honored as-is.
absl::StatusOr<CliffRow> RunOneCorpusCell(const CliffConfig& config,
                                          absl::string_view condition,
                                          const CliffCorpusCase& corpus_case,
                                          uint64_t memory_budget_chars,
                                          uint32_t repeat_idx);

}  // namespace litert::lm::bench

#endif  // THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_DPM_PROJECTION_CLIFF_H_
