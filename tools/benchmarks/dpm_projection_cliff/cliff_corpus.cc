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

#include "tools/benchmarks/dpm_projection_cliff/cliff_corpus.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/numbers.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "absl/strings/strip.h"  // from @com_google_absl

namespace litert::lm::bench {
namespace {

// Returns the indent (count of leading spaces) of `line`. Tabs are
// rejected upstream — the case YAML is space-indented.
int IndentOf(absl::string_view line) {
  int n = 0;
  while (n < static_cast<int>(line.size()) && line[n] == ' ') ++n;
  return n;
}

bool IsBlankOrComment(absl::string_view line) {
  const auto stripped = absl::StripAsciiWhitespace(line);
  return stripped.empty() || stripped.front() == '#';
}

// Strips an optional surrounding quote pair. YAML tolerates
//   key: "value"  | key: 'value' | key: value
// and we want the unquoted text.
std::string StripQuotes(absl::string_view s) {
  s = absl::StripAsciiWhitespace(s);
  if (s.size() >= 2 &&
      ((s.front() == '"' && s.back() == '"') ||
       (s.front() == '\'' && s.back() == '\''))) {
    return std::string(s.substr(1, s.size() - 2));
  }
  return std::string(s);
}

// Parses a flow-style list "[a, b, c]" into individual entries. Quotes
// are stripped per element. Returns the entries.
std::vector<std::string> ParseFlowList(absl::string_view body) {
  std::vector<std::string> out;
  body = absl::StripAsciiWhitespace(body);
  if (body.size() < 2 || body.front() != '[' || body.back() != ']') {
    return out;  // not a flow list — let the caller fall back
  }
  absl::string_view inner = body.substr(1, body.size() - 2);
  std::string current;
  bool in_quote = false;
  char quote_char = '\0';
  for (char c : inner) {
    if (in_quote) {
      if (c == quote_char) {
        in_quote = false;
      } else {
        current.push_back(c);
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      in_quote = true;
      quote_char = c;
      continue;
    }
    if (c == ',') {
      out.push_back(std::string(absl::StripAsciiWhitespace(current)));
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    out.push_back(std::string(absl::StripAsciiWhitespace(current)));
  }
  return out;
}

// Whole-file load + line split. Strips trailing CR from CRLF line
// endings so Windows-authored case files load identically to Unix ones.
absl::StatusOr<std::vector<std::string>> ReadLines(
    absl::string_view yaml_path) {
  std::ifstream in{std::string(yaml_path)};
  if (!in.is_open()) {
    return absl::NotFoundError(
        absl::StrCat("LoadCorpusCase: cannot open ", yaml_path));
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
  }
  return lines;
}

// Reads a `key: |` block scalar starting at lines[idx+1]. Returns the
// joined content (newline-preserving) and the index of the first line
// AFTER the block. Block ends when a line's indent is <= base_indent
// and the line is not blank. Empty lines inside a block are kept.
struct BlockScalar {
  std::string value;
  size_t next_idx;
};
BlockScalar ReadBlockScalar(const std::vector<std::string>& lines,
                            size_t idx, int base_indent) {
  BlockScalar out;
  size_t i = idx + 1;
  // First non-blank line determines the block's content indent.
  int content_indent = -1;
  while (i < lines.size()) {
    const std::string& l = lines[i];
    if (IsBlankOrComment(l)) {
      out.value.push_back('\n');
      ++i;
      continue;
    }
    const int ind = IndentOf(l);
    if (ind <= base_indent) break;
    if (content_indent < 0) content_indent = ind;
    out.value.append(l.substr(std::min<size_t>(content_indent, l.size())));
    out.value.push_back('\n');
    ++i;
  }
  out.next_idx = i;
  return out;
}

// Routes a parsed probe map into one of the four CliffGroundTruth
// slots, populating the matcher hint based on which expected_* key was
// supplied.
struct ParsedProbe {
  std::string axis;
  std::string question;
  std::string expected_exact;
  std::vector<std::string> expected_substrings;
  std::string judge_rubric;
};

void RouteProbe(const ParsedProbe& p, CliffGroundTruth* gt,
                std::string* rcs_rubric, std::string* crr_rubric) {
  CliffProbe target;
  target.question = p.question;
  if (!p.expected_exact.empty()) {
    target.required_substrings = {p.expected_exact};
    target.all_required = true;
    target.deterministic = true;
    target.matcher_kind = CliffProbe::kStringSubstring;
  } else if (!p.expected_substrings.empty()) {
    target.required_substrings = p.expected_substrings;
    target.all_required = true;
    target.deterministic = true;
    target.matcher_kind = CliffProbe::kStringSubstring;
  } else {
    // Judge-scored axis with no deterministic ground truth. Leave
    // required_substrings empty so the local scorer omits this axis;
    // the rubric is surfaced via the corpus-case so an external judge
    // can fill it later.
    target.required_substrings.clear();
    target.deterministic = false;
  }

  if (p.axis == "frp") {
    // FRP probes that ask for numeric anchors get the digit-only
    // matcher; substring probes (technique IDs etc) keep string match.
    bool looks_numeric = false;
    for (const std::string& s : target.required_substrings) {
      if (!s.empty() && std::isdigit(static_cast<unsigned char>(s[0]))) {
        looks_numeric = true;
        break;
      }
    }
    if (looks_numeric) target.matcher_kind = CliffProbe::kFrpNumeric;
    gt->frp = std::move(target);
  } else if (p.axis == "rcs") {
    *rcs_rubric = p.judge_rubric;
    gt->rcs = std::move(target);
  } else if (p.axis == "eda") {
    gt->eda = std::move(target);
  } else if (p.axis == "crr") {
    *crr_rubric = p.judge_rubric;
    gt->crr = std::move(target);
  }
}

}  // namespace

absl::StatusOr<CliffCorpusCase> LoadCorpusCase(
    absl::string_view yaml_path) {
  auto lines_or = ReadLines(yaml_path);
  if (!lines_or.ok()) return lines_or.status();
  const std::vector<std::string>& lines = *lines_or;

  CliffCorpusCase out;
  // Section state: TOP, TRAJECTORY, GROUND_TRUTH, PROBES.
  enum Section { kTop, kTrajectory, kGroundTruth, kProbes } section = kTop;

  // Trajectory-event accumulator.
  int event_index = 1;
  struct PartialEvent {
    std::string type;
    std::string timestamp;
    std::string payload;
  };
  PartialEvent cur;
  bool cur_active = false;
  auto flush_event = [&]() {
    if (!cur_active) return;
    // Classify this event for the checkpoint policy + add the
    // structured ClassifiedEvent to the case's events vector.
    ClassifiedEvent ce;
    ce.type_bucket = ClassifyEventType(cur.type, cur.payload);
    // Render in the "[N] {json-ish}\n" shape matching the synthetic
    // generator so RealRow's prompt-building works unchanged.
    std::ostringstream oss;
    oss << "[" << event_index++ << "] {\"type\":\"" << cur.type << "\","
        << "\"timestamp\":\"" << cur.timestamp << "\","
        << "\"payload\":";
    // JSON-escape payload string.
    oss << "\"";
    for (char c : cur.payload) {
      switch (c) {
        case '\\': oss << "\\\\"; break;
        case '"': oss << "\\\""; break;
        case '\n': oss << "\\n"; break;
        case '\r': oss << "\\r"; break;
        case '\t': oss << "\\t"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x",
                          static_cast<unsigned>(c) & 0xff);
            oss << buf;
          } else {
            oss << c;
          }
      }
    }
    oss << "\"}\n";
    const std::string rendered = oss.str();
    out.event_log.append(rendered);
    ce.text = rendered;
    ce.approx_tokens = static_cast<int>(rendered.size() / 4);
    out.events.push_back(std::move(ce));
    cur = PartialEvent{};
    cur_active = false;
  };

  // Probe accumulator.
  ParsedProbe probe;
  bool probe_active = false;
  auto flush_probe = [&]() {
    if (!probe_active) return;
    RouteProbe(probe, &out.ground_truth, &out.rcs_rubric, &out.crr_rubric);
    probe = ParsedProbe{};
    probe_active = false;
  };

  for (size_t i = 0; i < lines.size();) {
    const std::string& line = lines[i];
    if (IsBlankOrComment(line)) { ++i; continue; }
    const int indent = IndentOf(line);
    const absl::string_view body = absl::StripAsciiWhitespace(line);

    // --- Top-level keys (indent 0). ---
    if (indent == 0) {
      flush_event();
      flush_probe();
      const auto colon = body.find(':');
      if (colon == absl::string_view::npos) { ++i; continue; }
      const auto key = absl::StripAsciiWhitespace(body.substr(0, colon));
      const auto rhs = absl::StripAsciiWhitespace(body.substr(colon + 1));

      if (key == "case_id") {
        out.case_id = StripQuotes(rhs);
        section = kTop;
      } else if (key == "domain") {
        out.domain = StripQuotes(rhs);
        section = kTop;
      } else if (key == "seed") {
        uint64_t v = 0;
        if (absl::SimpleAtoi(rhs, &v)) out.seed = v;
        section = kTop;
      } else if (key == "trajectory") {
        section = kTrajectory;
      } else if (key == "ground_truth") {
        section = kGroundTruth;
      } else if (key == "probes") {
        section = kProbes;
      } else {
        section = kTop;
      }
      ++i;
      continue;
    }

    // --- TRAJECTORY section: list of events. ---
    if (section == kTrajectory) {
      // List item header `- type: ...`.
      if (body.substr(0, 2) == "- ") {
        flush_event();
        cur_active = true;
        const auto rest = body.substr(2);
        const auto colon = rest.find(':');
        if (colon != absl::string_view::npos) {
          const auto k = absl::StripAsciiWhitespace(rest.substr(0, colon));
          const auto v =
              absl::StripAsciiWhitespace(rest.substr(colon + 1));
          if (k == "type") cur.type = StripQuotes(v);
          else if (k == "timestamp") cur.timestamp = StripQuotes(v);
          else if (k == "payload") {
            // Expect the next lines to be a `|` block.
            if (v == "|") {
              auto block = ReadBlockScalar(lines, i, indent);
              cur.payload = block.value;
              i = block.next_idx;
              continue;
            } else {
              cur.payload = StripQuotes(v);
            }
          }
        }
        ++i;
        continue;
      }
      // Continuation key for the active event.
      const auto colon = body.find(':');
      if (colon == absl::string_view::npos) { ++i; continue; }
      const auto k = absl::StripAsciiWhitespace(body.substr(0, colon));
      const auto v = absl::StripAsciiWhitespace(body.substr(colon + 1));
      if (k == "type") cur.type = StripQuotes(v);
      else if (k == "timestamp") cur.timestamp = StripQuotes(v);
      else if (k == "payload") {
        if (v == "|") {
          auto block = ReadBlockScalar(lines, i, indent);
          cur.payload = block.value;
          i = block.next_idx;
          continue;
        } else {
          cur.payload = StripQuotes(v);
        }
      }
      ++i;
      continue;
    }

    // --- GROUND_TRUTH section. ---
    if (section == kGroundTruth) {
      const auto colon = body.find(':');
      if (colon == absl::string_view::npos) { ++i; continue; }
      const auto k = absl::StripAsciiWhitespace(body.substr(0, colon));
      const auto v = absl::StripAsciiWhitespace(body.substr(colon + 1));

      if (body.substr(0, 2) == "- ") {
        // List entry inside reasoning_anchors.
        out.reasoning_anchors.push_back(StripQuotes(body.substr(2)));
      } else if (k == "decision_label") {
        out.decision_label = StripQuotes(v);
      } else if (k == "citation") {
        out.citation = StripQuotes(v);
      } else if (k == "reasoning_anchors") {
        if (!v.empty() && v.front() == '[') {
          for (auto& s : ParseFlowList(v)) {
            out.reasoning_anchors.push_back(StripQuotes(s));
          }
        }
        // else: subsequent `- ` items handled above
      }
      ++i;
      continue;
    }

    // --- PROBES section: list of {axis, question, expected_*, rubric}. ---
    if (section == kProbes) {
      if (body.substr(0, 2) == "- ") {
        flush_probe();
        probe_active = true;
        const auto rest = body.substr(2);
        const auto colon = rest.find(':');
        if (colon != absl::string_view::npos) {
          const auto k = absl::StripAsciiWhitespace(rest.substr(0, colon));
          const auto v =
              absl::StripAsciiWhitespace(rest.substr(colon + 1));
          if (k == "axis") probe.axis = StripQuotes(v);
        }
        ++i;
        continue;
      }
      const auto colon = body.find(':');
      if (colon == absl::string_view::npos) { ++i; continue; }
      const auto k = absl::StripAsciiWhitespace(body.substr(0, colon));
      const auto v = absl::StripAsciiWhitespace(body.substr(colon + 1));
      if (k == "axis") {
        probe.axis = StripQuotes(v);
      } else if (k == "question") {
        if (v == "|") {
          auto block = ReadBlockScalar(lines, i, indent);
          probe.question = absl::StripAsciiWhitespace(block.value);
          i = block.next_idx;
          continue;
        } else {
          probe.question = StripQuotes(v);
        }
      } else if (k == "expected_exact") {
        probe.expected_exact = StripQuotes(v);
      } else if (k == "expected_substrings") {
        if (!v.empty() && v.front() == '[') {
          for (auto& s : ParseFlowList(v)) {
            probe.expected_substrings.push_back(StripQuotes(s));
          }
        }
        // else: collected via subsequent `- ` lines below
      } else if (k == "judge_rubric") {
        if (v == "|") {
          auto block = ReadBlockScalar(lines, i, indent);
          probe.judge_rubric = absl::StripAsciiWhitespace(block.value);
          i = block.next_idx;
          continue;
        } else {
          probe.judge_rubric = StripQuotes(v);
        }
      } else if (body.substr(0, 2) == "- ") {
        // String item inside an `expected_substrings:` list block.
        probe.expected_substrings.push_back(StripQuotes(body.substr(2)));
      }
      ++i;
      continue;
    }

    ++i;
  }
  flush_event();
  flush_probe();

  if (out.case_id.empty()) {
    return absl::InvalidArgumentError(
        absl::StrCat("LoadCorpusCase: ", yaml_path, " has no case_id."));
  }
  if (out.event_log.empty()) {
    return absl::InvalidArgumentError(
        absl::StrCat("LoadCorpusCase: ", yaml_path,
                     " has empty trajectory."));
  }
  return out;
}

absl::StatusOr<std::vector<std::string>> ListCasePaths(
    absl::string_view corpus_dir) {
  std::vector<std::string> paths;
  std::error_code ec;
  if (!std::filesystem::exists(std::string(corpus_dir), ec)) {
    return absl::NotFoundError(absl::StrCat("ListCasePaths: ", corpus_dir,
                                            " does not exist"));
  }
  for (const auto& entry :
       std::filesystem::directory_iterator(std::string(corpus_dir), ec)) {
    if (ec) {
      return absl::InternalError(absl::StrCat("ListCasePaths: ",
                                              ec.message()));
    }
    if (!entry.is_regular_file()) continue;
    const auto p = entry.path();
    if (p.extension() != ".yaml" && p.extension() != ".yml") continue;
    paths.push_back(p.string());
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

}  // namespace litert::lm::bench
