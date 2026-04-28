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

#ifndef THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_CLIFF_TRAJECTORY_H_
#define THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_CLIFF_TRAJECTORY_H_

#include <cstdint>
#include <string>
#include <vector>

namespace litert::lm::bench {

// A single probe question asked at the end of the trajectory plus the
// ground truth a correct response must contain.
struct CliffProbe {
  // Free-text question appended to the projected prompt.
  std::string question;
  // Substrings (or alternates) that must appear in the model response
  // for the probe to score 1.0.
  //
  //   - matcher_kind == kFrpNumeric: digit-run comparison (commas /
  //     currency symbols ignored). Every entry must match.
  //   - all_required && matcher_kind == kStringSubstring:
  //     accept a *majority* of substrings (ceil(n/2)) so the model
  //     gets credit for recalling the gist of a reasoning chain even
  //     if one fact is paraphrased.
  //   - !all_required: any one substring suffices.
  enum MatcherKind {
    kStringSubstring = 0,
    kFrpNumeric = 1,
  };
  std::vector<std::string> required_substrings;
  bool all_required = true;
  // False for judge-only probes, where the local harness should carry
  // the question/rubric through but must not score the axis as a miss.
  bool deterministic = false;
  MatcherKind matcher_kind = kStringSubstring;
};

// Decision-alignment ground truth. Each axis has exactly one probe and
// a binary score (1.0 if the probe is satisfied, 0.0 otherwise). The
// composite decision_score is *not* stored here; CliffRow recomputes it
// from the four axes (or leaves it unset if all four are present).
struct CliffGroundTruth {
  CliffProbe frp;  // Factual recall (specific value).
  CliffProbe rcs;  // Reasoning chain (multi-fact join).
  CliffProbe eda;  // Decision accuracy (action / disposition).
  CliffProbe crr;  // Compliance reconstruction (regulation cite).
};

// Output of the trajectory generator: the synthetic event log the
// driver will feed through DPM, plus the ground truth used to score
// the model response after replay.
struct CliffTrajectory {
  std::string event_log;          // Newline-delimited events.
  CliffGroundTruth ground_truth;  // Probes + expected substrings.
  std::string anchor_case_id;     // Case ID the probes reference.
  uint64_t needle_positions[4] = {0, 0, 0, 0};  // Char offsets of the
                                                 // FRP/RCS/EDA/CRR
                                                 // needles in event_log.
};

// Builds a deterministic synthetic trajectory of approximately
// `target_chars` characters. The seed is forwarded into the RNG so two
// calls with identical (seed, target_chars) produce byte-identical
// output. Needles are planted at fixed quartiles; padding events fill
// the rest with similar-looking case data so the model has to actually
// retrieve the right one rather than memorizing position.
CliffTrajectory BuildTrajectory(uint64_t seed, uint64_t target_chars);

// Concatenates the four probe questions into a single user prompt that
// can be appended after the projected memory. The order is FRP, RCS,
// EDA, CRR; each probe is numbered so the response can be parsed.
std::string ComposeProbePrompt(const CliffGroundTruth& gt);

// Scores a model response against the ground truth on the four axes.
// Each axis is 1.0 (probe satisfied) or 0.0 (not). The matching is
// case-insensitive substring matching with whitespace normalization.
struct CliffScores {
  double frp = 0.0;
  double rcs = 0.0;
  double eda = 0.0;
  double crr = 0.0;
};
CliffScores ScoreResponse(const std::string& response,
                          const CliffGroundTruth& ground_truth);

}  // namespace litert::lm::bench

#endif  // THIRD_PARTY_ODML_LITERT_LM_TOOLS_BENCHMARKS_DPM_PROJECTION_CLIFF_CLIFF_TRAJECTORY_H_
