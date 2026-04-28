# Phase 2 Substrate Test Matrix

The substrate's correctness story rests on six **properties** the
primitives must hold. Each property is checked against three **corpora**:
the paper's synthetic LongHorizon-Bench cases (academic credibility),
real agent-session captures (real-user signal), and pinned regression
fixtures (lock-in-known-good).

This is the matrix the engineer is filling. Read this before writing any
test body. Each cell below is one `TEST_F` (or parameterized variant).

```
                   synth-corpus     real-sessions    regression-fixtures
                   ──────────────   ──────────────   ────────────────────
P1 replay-determ   [done]           [engineer]       [engineer]
P2 manifest-auth   [done]           [engineer]       [engineer]
P3 merkle-integ    [done]           [engineer]       [engineer]
P4 range-coverage  [done]           [engineer]       [engineer]
P5 replay-from-raw [done]           [engineer]       [engineer]
P6 cross-context   [done]           [engineer]       [engineer]
```

The "synth-corpus" column is satisfied by `MakeEvents(N)` in the test
file — a deterministic event generator that exercises each property
end-to-end with synthetic-but-realistic data. Real-sessions and
regression-fixtures columns remain engineer-todo and need:
  - real-sessions: thread the `SessionCase` JSON loader (lives on
    `phase2-bench`) into the test fixture, parameterize each property
    test on a `SessionCase`. This requires either cherry-picking the
    loader onto `phase2-substrate` or moving the corresponding tests
    to `phase2-bench`.
  - regression-fixtures: take a current passing run, freeze the
    expected hashes / states into a `regression_fixtures/` JSON file,
    and add a TEST_F that asserts byte-equality against the frozen
    values. CI then catches any future substrate change that perturbs
    output.

Plus three orthogonal axes layered on top:

- **differential**: same probe under DPM vs rolling-summary vs raw-context.
- **stability**: re-run the same probe N times, assert byte-identical projection.
- **stress**: deep DAG, multi-thread writes, integrity-fail recovery.

---

## Property specs

### P1 — Replay determinism

> Same `(events, schema_id, model_id, T_task)` produces byte-identical
> projection bytes across N replays.

**Why this matters to a user:** "every time I come back, the agent has
the same recall of what we discussed." Without P1, mental model drifts
between sessions and the user re-explains context constantly.

**Primitive surface:** `CreateProjectionPromptParts(...)` +
`ComputeManifestHash(algo, input)`.

**Assert:**
- Hash of `parts.cacheable_prefix` is stable across N=10 calls.
- Hash of `parts.event_log_suffix` is stable across N=10 calls.
- For corpora with deterministic ground truth, the projection bytes
  match the golden hash.

**Failure means:** the substrate's deterministic-replay claim is false.
Stop here; nothing else holds.

**Status:** [done in C++] paper-corpus cell.
**Engineer todo:** real-sessions + regression-fixtures cells.

---

### P2 — Manifest authority

> Mutating any field of `CanonicalManifestInput` outside the
> `EncodeCanonicalManifest` API breaks `ComputeManifestHash` parity.

**Why this matters to a user:** if a buggy retry, a concurrent agent,
or a flipped disk bit corrupts a manifest, the substrate detects it
instead of answering confidently from torn state.

**Primitive surface:** `EncodeCanonicalManifest`,
`ComputeManifestHash`, `Hash256`.

**Assert:**
- Compute hash. Mutate one byte of the input struct (e.g.,
  `body_size_bytes++`). Recompute. Assert the two hashes differ.
- Repeat for each field that lives in the canonical encoding.

**Failure means:** auditing claims are theater — silent mutation goes
undetected.

**Status:** [done in C++] paper-corpus cell.
**Engineer todo:** real-sessions + regression-fixtures cells.

---

### P3 — Merkle integrity

> A 5-leaf chain through `MerkleDagStore` validates as a whole; mutating
> one leaf body invalidates the root through any walk path.

**Why this matters to a user:** flaky disk corrupts one checkpoint
file; the agent's pickup explicitly errors `CHECKPOINT_CORRUPT` rather
than continuing on partial state.

**Primitive surface:** `MerkleDagStore::Put`, `MerkleDagStore::Get`,
`MerkleDagStore::GetCheckpointProvenance`.

**Assert:**
- Build a 5-leaf chain (each leaf references the previous one's hash).
- Walk the chain via `GetCheckpointProvenance`; assert successful BFS
  topological ordering.
- Mutate one leaf's body off-store; re-load; assert the next walk
  returns an error (or non-OK status with sufficient detail).

**Failure means:** corruption goes silent. Audit trail is unreliable.

**Status:** [engineer]

---

### P4 — Range coverage

> Given a 100-event log split into N checkpoints, every event is
> covered by exactly one manifest's `[base_event_index, base_event_index +
> coverage_count)` range. No gaps. No overlaps.

**Why this matters to a user:** "you forgot we discussed Y on Tuesday
afternoon." Coverage holes in the chain become memory holes.

**Primitive surface:** `CanonicalManifestInput::base_event_index` +
implicit `coverage_count` (engineer must wire this if not already
present; document the addition in the test commit).

**Assert:**
- Project a 100-event log into 10 checkpoints.
- Iterate manifests in DAG order; collect their event ranges.
- Assert union of ranges = [0, 100), pairwise disjoint.

**Failure means:** silent skipping of events. Memory gaps for the user.

**Status:** [engineer]

---

### P5 — Replay-from-raw recomputation

> Given a stored manifest + the raw events in its event range + the
> recorded schema_id + the recorded model_id, re-projection produces
> the same `body_hash` as the stored manifest.

**Why this matters to a user:** "show me how you arrived at that." The
agent can replay the raw events and prove the projection state at that
moment was derivable from observable inputs alone — no hallucinated
rationale.

**Primitive surface:** `CreateProjectionPromptParts` (with the recorded
schema_id and event_log_suffix), `ComputeManifestHash`.

**Assert:**
- Pick a stored manifest M (from any corpus).
- Reconstruct the event range from the raw log.
- Re-project with the manifest's recorded schema/model.
- Assert `ComputeManifestHash(...)` of the recomputation equals
  `M.manifest_hash`.

**This is the killer test.** If it fails, "deterministic replay" doesn't
actually hold.

**Failure means:** the substrate cannot reconstruct from raw events.
Replay is unprovable.

**Status:** [engineer]

---

### P6 — Cross-context roll-up

> Two `CheckpointStore` instances backed by different filesystem roots
> (or, in the bench tier, different processes) can resume each other's
> manifest chains by hash. The roll-up of leaf manifests on instance B
> is byte-identical to the roll-up on instance A.

**Why this matters to a user:** laptop crashes mid-session; agent on
phone picks up. Or: agent on the GPU box runs overnight; agent on the
desktop reads its state in the morning.

**Primitive surface:** `LocalFilesystemCheckpointStore` (instance A and
B), `MerkleDagStore` (cross-instance hash references).

**Assert:**
- Process A writes 10 leaf manifests + 1 root.
- Process B (different `tmpdir`) reads the manifests by hash.
- Process B re-derives the root from leaves.
- Assert B's root hash equals A's root hash.

**Failure means:** substrate is process-local. The "scale beyond one
context window" claim doesn't hold.

**Status:** [engineer]

---

## Corpus columns

### paper-corpus

Synthetic cases derived from the paper's `cases_large.py` template
(loan + claim, ~27 K chars each, three budgets). The reference for
academic-credibility comparisons. Used by P1, P2, P5 for byte-identity
assertions against frozen-good outputs.

**Where it comes from:** `tools/benchmarks/dpm_projection_cliff/cliff_trajectory.cc`
synthesizes paper-shaped cases. The test fixture treats them as
parameter sets, not files; no JSON corpus here.

### real-sessions

`SessionCase` records produced by
`tools/benchmarks/dpm_projection_cliff/scenario/ingest_session.py`
from the user's actual Claude / Codex session logs. The primary
"does this help real users" signal. Loaded via the
`SessionCaseLoader` (lives on `phase2-bench`).

**Where it comes from:** the engineer points the loader at JSON files
generated from session JSONLs. A small set of golden fixtures live in
`tools/benchmarks/dpm_projection_cliff/scenario/golden/` for tests
that should run without the user's logs available.

### regression-fixtures

A set of <10 frozen `SessionCase` JSONs checked into
`runtime/platform/checkpoint/testing/regression_fixtures/`.
Hand-curated by the engineer. Each fixture pins a known-good behavior:
P1's frozen hash, P3's frozen mutation-detection, etc. If a future
substrate change causes any frozen hash to drift, the regression cell
fails loudly.

**Where it comes from:** the engineer generates them once, by
running the property tests against the current `phase2-substrate` head
and serializing the assertion-passing inputs and expected outputs.

---

## Differential / stability / stress (engineer)

These layer on top of the matrix. Each is one or more `TEST_F` cases,
not a per-cell expansion.

### differential

For every property test that produces a comparable output, run it
twice: once under DPM (`CreateProjectionPromptParts`), once under a
synthetic `RollingSummary` baseline (engineer must add a small
`rolling_summary_baseline.{h,cc}` that does N summarize calls and
returns a 200-word digest). Assert DPM's output is not strictly worse
than the baseline for the property under test (e.g., P1 must hold
under DPM; the baseline is allowed to fail P1 — that's the point).

### stability

For each property test, run N=100 replays. Assert byte-identity across
all replays. Catches subtle non-determinism (locale, iteration order,
unsorted maps).

### stress

- 10K-leaf Merkle DAG: P3 still detects a single-byte mutation.
- 8 concurrent writers to one CheckpointStore: P2 catches the last-
  write-wins collision.
- Deliberately corrupt one leaf body mid-walk: P5's recompute path
  reports the exact corrupted leaf, not just "something failed."

---

## How the engineer should sequence the work

1. **Read this doc.** All of it. Each "Failure means" line is the
   user-visible scenario the test prevents — write the test for that
   scenario, not just to pass the assertion.
2. **Branch from `phase2-substrate`.** Name: `phase2-substrate-property-tests`.
3. **Pick P3 first.** It's the most self-contained (uses
   `MerkleDagStore` directly, no projection needed). Validates that
   the matrix scaffold compiles before tackling the harder cells.
4. **Then P4, then P5.** P5 is the killer test; do it deliberately.
5. **P6 last.** Multi-process means temp-dir juggling, harder fixture.
6. **Once all six property tests are green on paper-corpus**, do the
   real-sessions column. The `SessionCaseLoader` (on `phase2-bench`)
   is your friend — load JSONs, drive the same primitives, assert the
   same properties.
7. **Then regression-fixtures.** Small. Boring. Important. Once they
   freeze a hash, the substrate can't drift without CI screaming.
8. **Then the orthogonal axes.** Differential first (it's a real
   product claim). Stability second. Stress last.

## How to confirm "Phase 2 holds"

When every cell of the matrix is green on `phase2-substrate`, the
answer to *"is Phase 2 primed for working?"* is yes-with-receipts.
Until then, the bench's decision-quality numbers can't be trusted as
substrate-correctness signal — they conflate two different claims.
