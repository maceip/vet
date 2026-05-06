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

#ifndef THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_CLIFF_CORPUS_H_
#define THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_CLIFF_CORPUS_H_

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "tools/benchmarks/dpm_projection_cliff/cliff_handoff.h"
#include "tools/benchmarks/dpm_projection_cliff/cliff_trajectory.h"

namespace litert::lm::bench {

// One hand-curated case loaded from a YAML file under corpora/cases/.
// The shape mirrors the LongHorizon-Bench-style schema agreed with the
// corpus author:
//
//   case_id: casinolimit-intrepide
//   domain: incident
//   seed: 20260426
//   trajectory:
//     - type: auditd_event
//       timestamp: 2024-05-17T19:56:11Z
//       payload: |
//         host=start-12 techniques=[T1033]
//         type=EXECVE msg=audit(...)
//     ...
//   ground_truth:
//     decision_label: STEP_2
//     citation: "CasinoLimit Manatee labels (RAID 2025)"
//     reasoning_anchors: [T1046, T1033, ...]
//   probes:
//     - axis: eda
//       question: |
//         Based on the trajectory, how far did this attacker progress...
//       expected_exact: STEP_2
//     - axis: frp
//       question: "Name two MITRE ATT&CK technique IDs observed..."
//       expected_substrings: [T1046, T1033]
//     - axis: rcs
//       question: "..."
//       judge_rubric: |
//         Score 1 if the answer mentions discovery/recon AND lateral...
//     - axis: crr
//       question: "..."
//       expected_substrings: [...]
struct CliffCorpusCase {
  std::string case_id;
  std::string domain;
  uint64_t seed = 0;

  // Concatenated event log in the same "[N] {...}\n" line format the
  // synthetic generator produces. CliffTrajectory.event_log is set to
  // this verbatim so the prompt-building paths in RealRow stay
  // architecture-agnostic across synthetic and corpus cases.
  std::string event_log;

  // Decision label (e.g. "STEP_2", "FLAG_CAPTURED", "APPROVE_FULL_REFUND")
  // and citation, both pulled from ground_truth. EDA probe defaults to
  // matching against decision_label; CRR probe defaults to matching
  // against citation. Per-probe expected_* in the YAML overrides
  // these defaults.
  std::string decision_label;
  std::string citation;

  // Reasoning anchors a correct rationale should reference. Surfaced in
  // the row for downstream judges; not used by the local scorer.
  std::vector<std::string> reasoning_anchors;

  // Probes routed into CliffGroundTruth's four axis slots by `axis`
  // value. A missing axis leaves the slot empty (CliffProbe with no
  // required_substrings, ProbeSatisfied returns false → score 0).
  CliffGroundTruth ground_truth;

  // Optional rubric strings for judge-scored axes. Empty when the
  // axis is gated against deterministic ground truth (FRP/EDA/CRR via
  // expected_*); non-empty for RCS-style axes that need an LLM judge.
  // The bench harness passes these through verbatim into the row's
  // scoring_misses fallback so an external judge pass can pick them up.
  std::string rcs_rubric;
  std::string crr_rubric;

  // Per-event classification used by the dpm_checkpoints_handoff
  // condition to drive the policy layer. Same length as the
  // trajectory; ordered. Each entry's `type_bucket` is the result of
  // ClassifyEventType; `text` is the rendered "[N] {json}" line that
  // ends up in event_log; `approx_tokens` is chars/4 for now.
  std::vector<ClassifiedEvent> events;
};

// Loads a single case from a YAML file. Returns InvalidArgument on
// malformed input. The parser is a small hand-rolled subset (top-level
// scalars, lists of mappings, block scalars with `|`, lists of strings)
// chosen so we don't pull yaml-cpp into the bench dep graph.
absl::StatusOr<CliffCorpusCase> LoadCorpusCase(absl::string_view yaml_path);

// Discovers all case files under a directory. Filenames matching
// "*.yaml" are loaded in lexical order so a sweep is reproducible.
// Empty directory returns an empty vector with OK status.
absl::StatusOr<std::vector<std::string>> ListCasePaths(
    absl::string_view corpus_dir);

}  // namespace litert::lm::bench

#endif  // THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_CLIFF_CORPUS_H_
