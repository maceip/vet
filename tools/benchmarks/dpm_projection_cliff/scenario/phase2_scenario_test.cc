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
//
// Phase 2 scenario tests — twins of the property tests on
// phase2-substrate, parameterized on real SessionCases. Each scenario
// asserts a user-visible behavior that depends on the underlying
// property. See runtime/platform/checkpoint/testing/PHASE2_TEST_MATRIX.md
// (on phase2-substrate) for the property-to-scenario mapping.
//
// Reference: S2 (next_tool_call) is fully implemented below as the
// pattern. S1, S3, S4, S5, S6 are stubs the engineer fills in by
// reading the matrix doc and the SessionCase JSON the corpus exposes.

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/match.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "gmock/gmock.h"  // from @com_google_googletest
#include "gtest/gtest.h"  // from @com_google_googletest

#include "runtime/dpm/projection_prompt.h"
#include "runtime/platform/hash/hasher.h"
#include "tools/benchmarks/dpm_projection_cliff/scenario/session_case_loader.h"

namespace litert::lm::bench {
namespace {

using ::testing::HasSubstr;

// The seed golden case that travels with the test. Engineer adds more
// goldens to the same dir; this fixture loads them all.
constexpr absl::string_view kSeedCasePath =
    "tools/benchmarks/dpm_projection_cliff/scenario/golden/seed_case.json";

// Minimum schema for the projection. Real scenarios will use the
// per-domain schema the trajectory demands; these tests use a simple
// shape so the substrate primitives exercise without per-domain noise.
constexpr absl::string_view kSchemaId = "session_replay_v1";
constexpr absl::string_view kSchemaJson =
    "{\"intent\":\"string\",\"open_decisions\":[],\"corrections\":[]}";
constexpr std::size_t kBudget = 5352;  // paper's moderate budget

// A test fixture that loads the seed golden once and exposes a typed
// SessionCase to each test. Engineer can extend this to load all
// golden files when they add more.
class Phase2SeedSessionFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    auto loaded = LoadSessionCasesFromFile(kSeedCasePath);
    ASSERT_TRUE(loaded.ok()) << loaded.status();
    ASSERT_FALSE(loaded->empty());
    cases_ = std::move(*loaded);
  }
  std::vector<SessionCase> cases_;
};

// ---------- S2 — next_tool_call (REFERENCE; fully implemented) ------

TEST_F(Phase2SeedSessionFixture, S2_NextToolCallProbeShapesProjection) {
  // Property under test: replay determinism + range coverage.
  // User scenario: when the user tees up an action ("now commit it"),
  // the agent's projection of session-so-far must include the prior
  // tool-call signal that determines what tool comes next. Specifically:
  // for the seed case, the projection should contain enough state that
  // a downstream test (when wired to a model) could ask "what tool is
  // about to be invoked, with what argument" and get the right answer.
  //
  // Here, with no model in the loop, we assert the substrate-level
  // pre-conditions:
  //   1. The projection bytes are deterministic (replay determinism).
  //   2. The projection's event_log_suffix contains the expected
  //      tool_name + arg_substring from the probe's ground truth
  //      — i.e. the substrate hasn't dropped that information.
  //
  // If a future substrate change starts truncating tool_name out of
  // the projection prompt, this test fails before any model run does.

  const SessionCase& c = cases_.front();
  // Find the next_tool_call probe.
  const SessionProbe* probe = nullptr;
  for (const auto& p : c.probes) {
    if (p.kind == "next_tool_call") { probe = &p; break; }
  }
  ASSERT_NE(probe, nullptr) << "seed case must contain a next_tool_call probe";

  // Render the events into a projection prompt the same way the bench
  // would on a real run. Hash twice; assert byte-equal (S1 / replay
  // determinism, scenario form).
  const std::string event_log = RenderEventLog(c);
  auto p1 = CreateProjectionPromptParts(event_log, kSchemaId, kSchemaJson,
                                        kBudget,
                                        /*max_event_log_chars=*/1 << 20);
  auto p2 = CreateProjectionPromptParts(event_log, kSchemaId, kSchemaJson,
                                        kBudget,
                                        /*max_event_log_chars=*/1 << 20);
  ASSERT_TRUE(p1.ok()) << p1.status();
  ASSERT_TRUE(p2.ok()) << p2.status();
  EXPECT_EQ(p1->Compose(), p2->Compose())
      << "Replay produced different projection bytes — substrate is "
         "non-deterministic for this case. Replay determinism violated.";

  // The expected tool name from the probe should appear somewhere in
  // the rendered event log (it was a real tool_call earlier in the
  // trajectory). If the ingester or the loader is dropping tool names
  // from event text, this catches it.
  ASSERT_FALSE(probe->expected_match.tool_name.empty())
      << "next_tool_call probe missing expected tool_name";
  EXPECT_THAT(event_log, HasSubstr(probe->expected_match.tool_name))
      << "Rendered event log dropped the expected tool name. "
         "The projection won't have enough signal for the agent to "
         "predict the next tool call.";
}

// ---------- S1 — next_user_intent (engineer stub) -------------------

TEST_F(Phase2SeedSessionFixture, S1_NextUserIntentProjectionByteIdentical) {
  GTEST_SKIP()
      << "engineer-todo S1: assert that re-rendering the projection "
      << "for the same (events_up_to_T, schema, budget) produces "
      << "byte-identical bytes; then (when paired with a model run) "
      << "the agent's predicted-next-user-utterance contains "
      << "probe.expected_match.substring. See PHASE2_TEST_MATRIX.md "
      << "§P1 for the property this scenario twins.";
}

// ---------- S3 — correction_detection (engineer stub) ---------------

TEST_F(Phase2SeedSessionFixture, S3_CorrectionDetectionPreservedInProjection) {
  GTEST_SKIP()
      << "engineer-todo S3: assert that when the user issued a "
      << "correction at probe_T, the projected memory bytes still "
      << "contain probe.expected_match.correction_substring. The "
      << "scenario for P3 (Merkle integrity) is: a correction event "
      << "must remain attributable to a specific event_index, "
      << "verifiable via the manifest chain. See "
      << "PHASE2_TEST_MATRIX.md §P3.";
}

// ---------- S4 — range_coverage_via_session (engineer stub) ---------

TEST_F(Phase2SeedSessionFixture, S4_NoEventsSilentlyDroppedFromSession) {
  GTEST_SKIP()
      << "engineer-todo S4: render the event log; for each event in "
      << "the SessionCase, assert its event idx prefix appears in "
      << "the rendered log. If any are missing, the substrate has a "
      << "coverage hole (the user-visible failure: 'agent forgot we "
      << "discussed Y in the middle of the session'). See "
      << "PHASE2_TEST_MATRIX.md §P4.";
}

// ---------- S5 — replay_from_raw (engineer stub) --------------------

TEST_F(Phase2SeedSessionFixture, S5_ProjectionReproducibleFromRawEvents) {
  GTEST_SKIP()
      << "engineer-todo S5: round-trip a checkpoint manifest for the "
      << "seed case events 0..probe_T; reload its abi_bytes; "
      << "re-project from raw events with the recorded schema/model; "
      << "assert the recomputed body_hash equals the stored one. The "
      << "user-visible failure: 'show me how you arrived at that' "
      << "produces a rationale not actually traceable to events. See "
      << "PHASE2_TEST_MATRIX.md §P5.";
}

// ---------- S6 — cross_context (engineer stub) ----------------------

TEST_F(Phase2SeedSessionFixture, S6_SessionResumesAcrossStoreInstances) {
  GTEST_SKIP()
      << "engineer-todo S6: write the seed case's projection into "
      << "store A, copy A's directory to a fresh dir, instantiate "
      << "store B on the copy, GetManifest by hash, assert the "
      << "loaded manifest_hash equals A's. User-visible: "
      << "'phone -> desktop continuity'. See "
      << "PHASE2_TEST_MATRIX.md §P6.";
}

// ---------- Secondary validation: AgenticQwen twin-differential -----
//
// This is SECONDARY VALIDATION ONLY. AgenticQwen rows fit in one
// context window, so they cannot exercise Phase 2 cross-context /
// hierarchical / replay-from-raw / Merkle DAG claims. They DO test
// whether DPM's task-conditioned compression preserves policy-critical
// facts under tight memory budgets.
//
// The reference test loads a synthetic AgenticQwen-shaped twin pair
// (normal_path + hack_path linked by paired_case_id) and asserts
// substrate-level invariants the projection must preserve regardless
// of which path is being scored.

class AgenticQwenSecondaryValidationFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    // The synthetic seed lives next to the agentic_qwen adapter so
    // the C++ test can run without downloading the real corpus. The
    // adapter's Python output for this fixture is regenerated below
    // via the same shape (so both stay in lockstep when one changes).
    auto loaded = LoadSessionCasesFromFile(
        "tools/benchmarks/dpm_projection_cliff/agentic_qwen/golden/"
        "synthetic_seed_pair.json");
    ASSERT_TRUE(loaded.ok()) << loaded.status();
    ASSERT_EQ(loaded->size(), 2u)
        << "AgenticQwen seed must produce a normal+hack twin pair";
    cases_ = std::move(*loaded);
  }
  std::vector<SessionCase> cases_;
};

TEST_F(AgenticQwenSecondaryValidationFixture,
       Twin_NormalAndHackShareEventsAndPolicyConstraints) {
  // Property: the normal and hack twins share the events prefix
  // (they're projections of the same conversation up to probe_T) and
  // their rubric must_include lists share the same policy
  // constraints — only must_call_tools / must_not_call_tools differ.
  // This pins the differential-test invariant that DPM's projection
  // must preserve policy facts in BOTH paths.
  ASSERT_EQ(cases_.size(), 2u);
  const SessionCase* normal = nullptr;
  const SessionCase* hack = nullptr;
  for (const auto& c : cases_) {
    if (c.pair_role == "normal") normal = &c;
    if (c.pair_role == "hack") hack = &c;
  }
  ASSERT_NE(normal, nullptr);
  ASSERT_NE(hack, nullptr);
  EXPECT_EQ(normal->paired_case_id, hack->case_id);
  EXPECT_EQ(hack->paired_case_id, normal->case_id);
  EXPECT_EQ(normal->events.size(), hack->events.size());

  // Both twins must carry the same "must_include" policy facts; the
  // user-visible failure if this drifts: DPM's projection retains
  // policy constraints in one path but drops them in the other,
  // making the substrate's compression behavior path-dependent.
  ASSERT_FALSE(normal->probes.empty());
  ASSERT_FALSE(hack->probes.empty());
  EXPECT_EQ(normal->probes[0].rubric.must_include,
            hack->probes[0].rubric.must_include);

  // Tool-sequence rubrics differ by construction: normal lists tools
  // that MUST be called; hack lists tools that MUST NOT be called.
  EXPECT_FALSE(normal->probes[0].rubric.must_call_tools.empty())
      << "normal twin must specify must_call_tools";
  EXPECT_FALSE(hack->probes[0].rubric.must_not_call_tools.empty())
      << "hack twin must specify must_not_call_tools";
}

TEST_F(AgenticQwenSecondaryValidationFixture,
       Twin_RenderedEventLogsAreByteIdentical) {
  // Stronger invariant: since the twins are projections of the SAME
  // conversation up to probe_T, their RenderEventLog output must be
  // byte-identical. If they're not, the adapter or the loader is
  // perturbing the prefix between paths — and any downstream
  // projection-determinism test would fail spuriously.
  ASSERT_EQ(cases_.size(), 2u);
  EXPECT_EQ(RenderEventLog(cases_[0]), RenderEventLog(cases_[1]));
}

}  // namespace
}  // namespace litert::lm::bench
