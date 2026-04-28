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

#include "tools/benchmarks/dpm_projection_cliff/cliff_trajectory.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl

namespace litert::lm::bench {
namespace {

constexpr int kNumNeedles = 4;

std::string LowerNoSpaces(absl::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) continue;
    out.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

bool ContainsCi(absl::string_view haystack, absl::string_view needle) {
  // Case-insensitive substring search after stripping whitespace from
  // both sides. Matches "$100,000" against "the limit was $100,000.",
  // "$100,000 limit", and "limit:$100,000" alike.
  std::string h = LowerNoSpaces(haystack);
  std::string n = LowerNoSpaces(needle);
  if (n.empty()) return true;
  return h.find(n) != std::string::npos;
}

// Strips everything except [0-9.] — used to compare dollar amounts
// without being defeated by formatting drift (commas in wrong place,
// missing $ sign, currency symbols inserted by the model). The
// matcher returns true when every digit run in `needle` appears as a
// contiguous substring inside `haystack` after stripping.
bool ContainsDigitsCi(absl::string_view haystack, absl::string_view needle) {
  auto digits_only = [](absl::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
      if ((c >= '0' && c <= '9') || c == '.') out.push_back(c);
    }
    return out;
  };
  const std::string h = digits_only(haystack);
  const std::string n = digits_only(needle);
  if (n.empty()) return false;
  return h.find(n) != std::string::npos;
}

bool ProbeSatisfied(absl::string_view response, const CliffProbe& probe) {
  if (probe.required_substrings.empty()) return false;
  // Per peer feedback the matcher was too strict: FRP probes failed on
  // commas-in-wrong-place dollar amounts, and RCS probes failed when
  // the model recalled the reasoning correctly but skipped one of the
  // three required tokens. We now:
  //   - For probes flagged numeric (matcher_kind == kFrpNumeric),
  //     compare digit-runs only so $4,012,517.82 matches $4012517.82.
  //   - For all_required probes, accept majority match (>= ceil(n/2))
  //     instead of strict all. This penalises a model that fully
  //     fabricates a chain but credits one that recalls the gist.
  if (probe.matcher_kind == CliffProbe::kFrpNumeric) {
    for (const std::string& s : probe.required_substrings) {
      if (!ContainsDigitsCi(response, s)) return false;
    }
    return true;
  }
  if (probe.all_required) {
    int hits = 0;
    for (const std::string& s : probe.required_substrings) {
      if (ContainsCi(response, s)) ++hits;
    }
    const int n = static_cast<int>(probe.required_substrings.size());
    const int needed = (n + 1) / 2;  // ceil(n/2): 1 of 1, 2 of 2-3, 3 of 4-5
    return hits >= needed;
  }
  for (const std::string& s : probe.required_substrings) {
    if (ContainsCi(response, s)) return true;
  }
  return false;
}

// Vocabulary used to fill out padding events. Picked to look like real
// insurance claim copy without leaking ground-truth needles.
constexpr std::array<const char*, 8> kPolicyAnchors = {
    "Regulation A section 1001.4",  "Regulation D section 4.20",
    "Regulation E section 1005.6",  "Regulation Z section 1026.36",
    "Regulation X section 1024.41", "Regulation V section 1022.52",
    "Regulation P section 1016.7",  "Regulation Q section 217.4",
};

constexpr std::array<const char*, 6> kProductLines = {
    "auto-collision", "homeowners-fire", "renters-theft",
    "marine-cargo",   "umbrella-tort",   "commercial-liability",
};

constexpr std::array<const char*, 6> kBucketActions = {
    "manual review", "auto approve", "request more info", "escalate",
    "deny",          "settle quick",
};

std::string PadEvent(int index, std::mt19937_64& rng) {
  std::uniform_int_distribution<int> pol(0, kPolicyAnchors.size() - 1);
  std::uniform_int_distribution<int> prod(0, kProductLines.size() - 1);
  std::uniform_int_distribution<int> action(0, kBucketActions.size() - 1);
  std::uniform_int_distribution<int> dollars(2500, 750000);
  std::uniform_int_distribution<int> deduct(100, 5000);
  std::ostringstream oss;
  oss << "[" << index << "] {\"type\":\"tool\","
      << "\"tenant_id\":\"bench-tenant\","
      << "\"session_id\":\"bench-session\","
      << "\"timestamp_us\":" << (1000000 + index) << ","
      << "\"payload\":\"CASE-" << index << " "
      << kProductLines[prod(rng)] << " policy limit $"
      << dollars(rng) << ", deductible $" << deduct(rng)
      << ", recommendation: " << kBucketActions[action(rng)]
      << ", anchor " << kPolicyAnchors[pol(rng)] << ".\"}\n";
  return oss.str();
}

// FRP needle: a specific dollar amount we will probe for verbatim.
// The anchor is "policy_limit_anomalous_$1234567.50 for CASE-NNNN".
struct FrpNeedle {
  std::string case_id;       // e.g. "CASE-2401"
  std::string policy_limit;  // e.g. "$1,372,418.50"
};

FrpNeedle MakeFrpNeedle(int case_id, std::mt19937_64& rng) {
  std::uniform_int_distribution<int> dollars(1000000, 9999999);
  std::uniform_int_distribution<int> cents(0, 99);
  const int d = dollars(rng);
  const int c = cents(rng);
  std::ostringstream amount;
  amount << "$" << (d / 1000) << "," << (d % 1000 < 100 ? "0" : "")
         << (d % 1000 < 10 ? "0" : "") << (d % 1000) << "."
         << (c < 10 ? "0" : "") << c;
  return {absl::StrCat("CASE-", case_id), amount.str()};
}

std::string FrpNeedleEvent(int index, const FrpNeedle& n) {
  std::ostringstream oss;
  oss << "[" << index << "] {\"type\":\"tool\","
      << "\"tenant_id\":\"bench-tenant\","
      << "\"session_id\":\"bench-session\","
      << "\"timestamp_us\":" << (1000000 + index) << ","
      << "\"payload\":\"FRP-ANCHOR " << n.case_id
      << " has policy limit " << n.policy_limit
      << " on file. Repeat: limit " << n.policy_limit << ".\"}\n";
  return oss.str();
}

// RCS needle: three fact tokens that must all appear in the response
// for the probe to score 1.0. The needle event ties them together.
struct RcsNeedle {
  std::string case_id;
  std::string cause;    // e.g. "ladder collapse"
  std::string action;   // e.g. "called Apex Roofing"
  std::string outcome;  // e.g. "denied for negligence"
};

RcsNeedle MakeRcsNeedle(int case_id) {
  return {absl::StrCat("CASE-", case_id), "ladder collapse",
          "called Apex Roofing on Tuesday",
          "denied for contractor negligence"};
}

std::string RcsNeedleEvent(int index, const RcsNeedle& n) {
  std::ostringstream oss;
  oss << "[" << index << "] {\"type\":\"correction\","
      << "\"tenant_id\":\"bench-tenant\","
      << "\"session_id\":\"bench-session\","
      << "\"timestamp_us\":" << (1000000 + index) << ","
      << "\"payload\":\"RCS-ANCHOR " << n.case_id
      << ": " << n.cause << " led to " << n.action << ", "
      << n.outcome << ". Hold this chain verbatim.\"}\n";
  return oss.str();
}

// EDA needle: a specific runbook recommendation tied to a case.
struct EdaNeedle {
  std::string case_id;
  std::string action;  // e.g. "ESCALATE-TO-LEGAL"
};

EdaNeedle MakeEdaNeedle(int case_id) {
  return {absl::StrCat("CASE-", case_id), "ESCALATE-TO-LEGAL"};
}

std::string EdaNeedleEvent(int index, const EdaNeedle& n) {
  std::ostringstream oss;
  oss << "[" << index << "] {\"type\":\"tool\","
      << "\"tenant_id\":\"bench-tenant\","
      << "\"session_id\":\"bench-session\","
      << "\"timestamp_us\":" << (1000000 + index) << ","
      << "\"payload\":\"EDA-ANCHOR " << n.case_id
      << " runbook recommendation: " << n.action
      << ". Operator must surface " << n.action
      << " in the disposition.\"}\n";
  return oss.str();
}

// CRR needle: a regulation citation that must appear in the response.
struct CrrNeedle {
  std::string case_id;
  std::string regulation;  // e.g. "Regulation BB section 1098.7"
};

CrrNeedle MakeCrrNeedle(int case_id) {
  return {absl::StrCat("CASE-", case_id),
          "Regulation BB section 1098.7"};
}

std::string CrrNeedleEvent(int index, const CrrNeedle& n) {
  std::ostringstream oss;
  oss << "[" << index << "] {\"type\":\"tool\","
      << "\"tenant_id\":\"bench-tenant\","
      << "\"session_id\":\"bench-session\","
      << "\"timestamp_us\":" << (1000000 + index) << ","
      << "\"payload\":\"CRR-ANCHOR " << n.case_id
      << " is governed by " << n.regulation
      << ". Cite the section verbatim in the disposition.\"}\n";
  return oss.str();
}

}  // namespace

CliffTrajectory BuildTrajectory(uint64_t seed, uint64_t target_chars) {
  std::seed_seq seq{seed, target_chars};
  std::mt19937_64 rng(seq);
  // Pick four distinct case ids that the needles will reference. We
  // bias them away from the trivial 1..4 range so the model can't latch
  // onto position.
  std::uniform_int_distribution<int> case_dist(2000, 9000);
  std::array<int, kNumNeedles> case_ids;
  for (int i = 0; i < kNumNeedles; ++i) {
    int candidate;
    bool unique;
    do {
      candidate = case_dist(rng);
      unique = true;
      for (int j = 0; j < i; ++j) {
        if (case_ids[j] == candidate) { unique = false; break; }
      }
    } while (!unique);
    case_ids[i] = candidate;
  }

  const FrpNeedle frp = MakeFrpNeedle(case_ids[0], rng);
  const RcsNeedle rcs = MakeRcsNeedle(case_ids[1]);
  const EdaNeedle eda = MakeEdaNeedle(case_ids[2]);
  const CrrNeedle crr = MakeCrrNeedle(case_ids[3]);

  // Quartile positions for needle insertion. We do not pin them at the
  // very start or end so the model can't shortcut by reading head/tail.
  const double frac[kNumNeedles] = {0.22, 0.45, 0.66, 0.85};

  CliffTrajectory traj;
  traj.anchor_case_id = absl::StrCat(
      "FRP=", frp.case_id, " RCS=", rcs.case_id,
      " EDA=", eda.case_id, " CRR=", crr.case_id);

  std::string& log = traj.event_log;
  log.reserve(target_chars + 2048);
  int index = 1;
  int needle_idx = 0;
  while (log.size() < target_chars) {
    // Insert needles at the planned positions.
    while (needle_idx < kNumNeedles &&
           log.size() >=
               static_cast<size_t>(frac[needle_idx] * target_chars)) {
      traj.needle_positions[needle_idx] = log.size();
      switch (needle_idx) {
        case 0: log += FrpNeedleEvent(index++, frp); break;
        case 1: log += RcsNeedleEvent(index++, rcs); break;
        case 2: log += EdaNeedleEvent(index++, eda); break;
        case 3: log += CrrNeedleEvent(index++, crr); break;
      }
      ++needle_idx;
    }
    log += PadEvent(index++, rng);
  }
  // Flush any remaining needles even if target_chars was undershot.
  while (needle_idx < kNumNeedles) {
    traj.needle_positions[needle_idx] = log.size();
    switch (needle_idx) {
      case 0: log += FrpNeedleEvent(index++, frp); break;
      case 1: log += RcsNeedleEvent(index++, rcs); break;
      case 2: log += EdaNeedleEvent(index++, eda); break;
      case 3: log += CrrNeedleEvent(index++, crr); break;
    }
    ++needle_idx;
  }

  // Probes — all reference the case ids planted above. The matcher is
  // case-insensitive whitespace-stripped substring; for FRP we accept
  // either the full $X,XXX,XXX.YY form or the digits-only form so a
  // well-aligned model can't be tripped by formatting drift.
  CliffGroundTruth gt;

  gt.frp.question = absl::StrCat(
      "Probe 1 (factual): What policy limit is on file for ",
      frp.case_id,
      "? Answer with the exact dollar amount and nothing else.");
  // Accept "$1,372,418.50" or "$1372418.50" — strip commas in matcher.
  std::string frp_no_comma = frp.policy_limit;
  frp_no_comma.erase(std::remove(frp_no_comma.begin(),
                                  frp_no_comma.end(), ','),
                     frp_no_comma.end());
  gt.frp.required_substrings = {frp_no_comma};
  gt.frp.all_required = true;
  gt.frp.deterministic = true;
  gt.frp.matcher_kind = CliffProbe::kFrpNumeric;

  gt.rcs.question = absl::StrCat(
      "Probe 2 (reasoning): Briefly summarize why ", rcs.case_id,
      " was sent to manual review. Answer in one sentence.");
  gt.rcs.required_substrings = {rcs.cause, "Apex Roofing",
                                "contractor negligence"};
  gt.rcs.all_required = true;
  gt.rcs.deterministic = true;

  gt.eda.question = absl::StrCat(
      "Probe 3 (decision): What disposition does the runbook "
      "recommend for ",
      eda.case_id,
      "? Answer with the recommendation token only.");
  gt.eda.required_substrings = {eda.action};
  gt.eda.all_required = true;
  gt.eda.deterministic = true;

  gt.crr.question = absl::StrCat(
      "Probe 4 (compliance): Cite the regulation that governs ",
      crr.case_id,
      ". Answer with the exact regulation citation.");
  gt.crr.required_substrings = {crr.regulation};
  gt.crr.all_required = true;
  gt.crr.deterministic = true;

  traj.ground_truth = std::move(gt);
  return traj;
}

std::string ComposeProbePrompt(const CliffGroundTruth& gt) {
  std::ostringstream oss;
  oss << "\n\nAnswer the following four probes. Number each answer "
         "(1, 2, 3, 4). Be terse — a single line per answer.\n\n"
      << gt.frp.question << "\n"
      << gt.rcs.question << "\n"
      << gt.eda.question << "\n"
      << gt.crr.question << "\n";
  return oss.str();
}

CliffScores ScoreResponse(const std::string& response,
                          const CliffGroundTruth& ground_truth) {
  CliffScores out;
  out.frp = ProbeSatisfied(response, ground_truth.frp) ? 1.0 : 0.0;
  out.rcs = ProbeSatisfied(response, ground_truth.rcs) ? 1.0 : 0.0;
  out.eda = ProbeSatisfied(response, ground_truth.eda) ? 1.0 : 0.0;
  out.crr = ProbeSatisfied(response, ground_truth.crr) ? 1.0 : 0.0;
  return out;
}

}  // namespace litert::lm::bench
