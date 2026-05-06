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

#include <fstream>
#include <iostream>
#include <string>

#include "absl/flags/flag.h"  // from @com_google_absl
#include "absl/flags/parse.h"  // from @com_google_absl
#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "tools/benchmarks/dpm_projection_cliff/cliff_corpus.h"
#include "tools/benchmarks/dpm_projection_cliff/dpm_projection_cliff.h"

#if defined(_WIN32)
extern "C" {
// Direct3D 12 Agility SDK sidecar selection. Dawn/WebGPU on Windows loads the
// bundled D3D12 runtime only when the host executable exports these symbols.
__declspec(dllexport) extern const unsigned int D3D12SDKVersion = 619;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
#endif

ABSL_FLAG(std::string, config, "",
          "Path to a benchmark config YAML (see configs/).");
ABSL_FLAG(std::string, output_jsonl, "",
          "Where to append JSONL rows. The driver appends; the file may "
          "already exist.");
ABSL_FLAG(bool, allow_mock, false,
          "Permit mock=true rows in the output. Off by default; the "
          "driver fails closed when --model_path is empty unless this "
          "flag is set so a stale build cannot silently produce mock "
          "rows that contaminate a real chart.");
ABSL_FLAG(std::string, model_path, "",
          "Pinned .litertlm model bundle. Required for real-substrate "
          "runs; leaving it empty falls back to the mock path (which "
          "still requires --allow_mock).");
ABSL_FLAG(std::string, backend, "cpu",
          "Backend to drive: cpu | gpu | npu | cpu_artisan | gpu_artisan.");
ABSL_FLAG(std::string, checkpoint_root, "",
          "Filesystem root under which dpm_checkpoints* conditions write "
          "their per-cell stores. Empty disables the CheckpointStore "
          "exercise (the conditions still produce decision rows but "
          "leave wall_clock_checkpoint_put_ms unset).");
ABSL_FLAG(std::string, checkpoint_backend, "local_fs",
          "Backend for dpm_checkpoints* conditions: "
          "'local_fs' (default; uses --checkpoint_root) or "
          "'s3_express' (uses --s3_bucket / --s3_az_id / --s3_region "
          "and AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY env vars).");
ABSL_FLAG(std::string, s3_bucket, "",
          "S3 Express bucket base name (without the --{az}--x-s3 "
          "suffix). Required when --checkpoint_backend=s3_express.");
ABSL_FLAG(std::string, s3_az_id, "",
          "S3 Express AZ ID, e.g. 'euc1-az1' for eu-central-1a. Must "
          "be the AZ the bucket is provisioned in.");
ABSL_FLAG(std::string, s3_region, "",
          "AWS region, e.g. 'eu-central-1'.");
ABSL_FLAG(int, max_num_tokens, 32768,
          "KV-cache capacity (and hard prompt cap) for each cell.");
ABSL_FLAG(int, safe_max_tokens, 12000,
          "Empirically validated prompt-token ceiling. Long CPU/Gemma4 "
          "contexts can crash in decode after successful prefill; prompts "
          "above this value emit a skipped row instead of calling decode. "
          "<=0 disables the safety gate for explicit repro/debug runs.");
ABSL_FLAG(int, prefill_chunk_size, 1024,
          "CPU dynamic-prefill chunk size. -1 keeps the model default; "
          "set explicitly (e.g. 1024) to bound each prefill iteration "
          "on CPU and dodge the long-context Gemma-4 segfault.");
ABSL_FLAG(int, num_cpu_threads, 0,
          "CPU/XNNPack thread count override. 0 keeps the model default.");
ABSL_FLAG(bool, conservative_gpu_settings, false,
          "Force serialized WebGPU upload/compile settings for debugging "
          "unstable Windows adapters. Off by default so GPU benchmark rows "
          "use LiteRT-LM's model-specific defaults.");
ABSL_FLAG(int, max_output_tokens, -1,
          "DEPRECATED. When > 0, overrides --decision_max_output_tokens.");
ABSL_FLAG(int, projection_max_output_tokens, -1,
          "Stage-1 (projection) decode budget. -1 derives from "
          "memory_budget_chars / 3 + 128, clamped to --max_num_tokens. "
          "Letting stage 1 run with the (tight) decision budget would "
          "clip the projection before any needle is preserved.");
ABSL_FLAG(int, decision_max_output_tokens, 256,
          "Stage-2 (decision) decode budget — has to fit the four probe "
          "answers plus a small format wrapper.");
ABSL_FLAG(std::string, corpus_dir, "",
          "Directory of hand-curated YAML cases (e.g. "
          "/home/devuser/corpora/cases/). When non-empty, the driver "
          "iterates one row per (case, condition, budget, repeat) "
          "instead of synthesizing trajectories from --config's "
          "trajectory_chars list. The case's event_log replaces the "
          "synthetic generator; case ground_truth + probes drive "
          "scoring. trajectory_chars in the YAML config is ignored in "
          "this mode.");
ABSL_FLAG(std::string, schema_id, "insurance_liability_v2",
          "Projection schema id used to frame each DPM cell's prompt.");
ABSL_FLAG(std::string, schema_json,
          R"json({"Facts":["string with one-based [i] citation"],)json"
          R"json("Reasoning":["string with one-based [i] citation"],)json"
          R"json("Compliance":["string with one-based [i] citation"]})json",
          "Projection schema JSON used to frame each DPM cell's prompt.");

namespace {

absl::Status RunMain() {
  const std::string config_path = absl::GetFlag(FLAGS_config);
  const std::string output_path = absl::GetFlag(FLAGS_output_jsonl);
  if (config_path.empty()) {
    return absl::InvalidArgumentError("--config is required.");
  }
  if (output_path.empty()) {
    return absl::InvalidArgumentError("--output_jsonl is required.");
  }
  auto cfg = litert::lm::bench::LoadConfig(config_path);
  if (!cfg.ok()) return cfg.status();
  cfg->allow_mock = absl::GetFlag(FLAGS_allow_mock);
  cfg->model_path = absl::GetFlag(FLAGS_model_path);
  cfg->backend = absl::GetFlag(FLAGS_backend);
  cfg->checkpoint_root = absl::GetFlag(FLAGS_checkpoint_root);
  cfg->checkpoint_backend = absl::GetFlag(FLAGS_checkpoint_backend);
  cfg->s3_bucket = absl::GetFlag(FLAGS_s3_bucket);
  cfg->s3_az_id = absl::GetFlag(FLAGS_s3_az_id);
  cfg->s3_region = absl::GetFlag(FLAGS_s3_region);
  cfg->max_num_tokens = absl::GetFlag(FLAGS_max_num_tokens);
  cfg->safe_max_tokens = absl::GetFlag(FLAGS_safe_max_tokens);
  cfg->prefill_chunk_size = absl::GetFlag(FLAGS_prefill_chunk_size);
  cfg->num_cpu_threads = absl::GetFlag(FLAGS_num_cpu_threads);
  cfg->conservative_gpu_settings =
      absl::GetFlag(FLAGS_conservative_gpu_settings);
  cfg->max_output_tokens = absl::GetFlag(FLAGS_max_output_tokens);
  cfg->projection_max_output_tokens =
      absl::GetFlag(FLAGS_projection_max_output_tokens);
  cfg->decision_max_output_tokens =
      absl::GetFlag(FLAGS_decision_max_output_tokens);
  cfg->schema_id = absl::GetFlag(FLAGS_schema_id);
  cfg->schema_json = absl::GetFlag(FLAGS_schema_json);
  cfg->config_path = config_path;
  cfg->corpus_dir = absl::GetFlag(FLAGS_corpus_dir);

  std::ofstream out(output_path, std::ios::out | std::ios::app);
  if (!out.is_open()) {
    return absl::InternalError(
        absl::StrCat("cannot open output ", output_path));
  }

  uint64_t emitted = 0;
  if (cfg->corpus_dir.empty()) {
    // Synthetic-trajectory mode (legacy behavior).
    for (const std::string& condition : cfg->conditions) {
      for (uint64_t traj : cfg->trajectory_chars) {
        for (uint64_t budget : cfg->memory_budget_chars) {
          for (uint32_t r = 0; r < cfg->repeats; ++r) {
            auto row = litert::lm::bench::RunOneCell(*cfg, condition, traj,
                                                     budget, r);
            if (!row.ok()) return row.status();
            out << litert::lm::bench::CliffRowToJsonl(*row) << "\n";
            out.flush();
            ++emitted;
            std::cerr << "[" << emitted << "] " << condition
                      << " traj=" << traj << " budget=" << budget
                      << " r=" << r << " score="
                      << row->decision_score.value_or(
                             row->deterministic_score.value_or(-1.0))
                      << "\n";
          }
        }
      }
    }
  } else {
    // Corpus mode: load every YAML case once, then iterate
    // (case, condition, budget, repeat). trajectory_chars from the
    // YAML config is ignored — each case has its own event_log size.
    auto paths = litert::lm::bench::ListCasePaths(cfg->corpus_dir);
    if (!paths.ok()) return paths.status();
    if (paths->empty()) {
      return absl::InvalidArgumentError(
          absl::StrCat("--corpus_dir contains no .yaml cases: ",
                       cfg->corpus_dir));
    }
    std::vector<litert::lm::bench::CliffCorpusCase> cases;
    cases.reserve(paths->size());
    for (const std::string& p : *paths) {
      auto c = litert::lm::bench::LoadCorpusCase(p);
      if (!c.ok()) {
        std::cerr << "[corpus] skipping " << p << ": " << c.status()
                  << "\n";
        continue;
      }
      cases.push_back(std::move(*c));
    }
    std::cerr << "[corpus] loaded " << cases.size() << " cases from "
              << cfg->corpus_dir << "\n";
    for (const std::string& condition : cfg->conditions) {
      for (uint64_t budget : cfg->memory_budget_chars) {
        for (uint32_t r = 0; r < cfg->repeats; ++r) {
          for (const auto& cs : cases) {
            auto row = litert::lm::bench::RunOneCorpusCell(
                *cfg, condition, cs, budget, r);
            if (!row.ok()) return row.status();
            out << litert::lm::bench::CliffRowToJsonl(*row) << "\n";
            out.flush();
            ++emitted;
            std::cerr << "[" << emitted << "] " << condition
                      << " case=" << cs.case_id << " budget=" << budget
                      << " r=" << r << " score="
                      << row->decision_score.value_or(
                             row->deterministic_score.value_or(-1.0))
                      << "\n";
          }
        }
      }
    }
  }
  out.flush();
  std::cerr << "wrote " << emitted << " rows to " << output_path << "\n";
  return absl::OkStatus();
}

}  // namespace

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  auto status = RunMain();
  if (!status.ok()) {
    std::cerr << status << "\n";
    return 1;
  }
  return 0;
}
