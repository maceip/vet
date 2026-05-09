"""Phase 3 bench row schema, enum lock, and chart guards.

This module is the **only** source of truth for what a Phase 3 bench row
looks like. Every other lane (fixtures / baselines / DPM substrate /
report) imports from here.

Why this exists: results without a schema-locked frame have shipped
before, and we ended up arguing about what the numbers meant after the
fact. This file makes a few specific failure modes structurally
impossible:

  1. Quality charts cannot include `prompt_retention` rows
     (those are a no-model diagnostic; they would inflate everything).

  2. Missing-judge rows are flagged `score_status = needs_judge` and are
     **never counted as zero** in aggregates.

  3. `dpm_phase3_checkpoint` rows must carry checkpoint hash + audit
     verdict + gate fields. A row that compares DPM to rolling-summary
     without those fields is rejected at construction time.

  4. Cross-condition fairness fields (model_id, budget_chars,
     final-decision prompt shape) are first-class — comparing
     conditions with mismatched budgets fails the chart guard.

Public API:

  BenchRow          -- the row dataclass
  Condition         -- enum: raw_oracle / rolling_summary /
                       dpm_phase3_checkpoint
  TestKind          -- enum: decision / handoff / correction_safety /
                       auditability / cost_latency / prompt_retention
  ScoreStatus       -- enum: scored / needs_judge /
                       skipped_context_too_large / errored
  AuditVerdict      -- enum mapping the C++ AuditVerdict (pass /
                       correction_emitted / inconclusive / pending) plus
                       NOT_APPLICABLE for non-DPM rows
  BenchRowError     -- raised on any schema violation

  validate_row(row) -> None             (raises BenchRowError on bad row)
  assert_quality_chart_rows(rows) -> list[BenchRow]
                       (rejects rows that should not enter quality
                        charts; raises BenchRowError if any disallowed
                        row is present)

  PHASE3_HANDOFF_QUALITY_CHART -- canonical ChartSpec for the headline
                                  decision/handoff/correction-safety
                                  panel.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field, asdict
from enum import Enum
from typing import Any, Iterable

SCHEMA_VERSION = 1


# ---- Enums (closed sets) ---------------------------------------------

class Condition(str, Enum):
    """The three substrate conditions under test.

    `raw_oracle` is the quality ceiling, not a deployable baseline.
    `rolling_summary` is the deployed-everywhere antagonist.
    `dpm_phase3_checkpoint` is the proposition; rows in this condition
    MUST carry checkpoint/audit/gate fields.
    """
    RAW_ORACLE = "raw_oracle"
    ROLLING_SUMMARY = "rolling_summary"
    DPM_PHASE3_CHECKPOINT = "dpm_phase3_checkpoint"


class TestKind(str, Enum):
    """What is being measured on this row.

    `decision`: answer correctness from memory.
    `handoff`: agent B resumes from memory only (no raw log).
    `correction_safety`: a correction invalidates a checkpoint or its
        ancestors; condition must refuse stale state or reproject.
    `auditability`: certificate + gate evidence is well-formed for the
        DPM condition.
    `cost_latency`: model calls + tokens + wall_ms.
    `prompt_retention`: no-model diagnostic. NEVER allowed in
        DPM-vs-rolling-summary quality charts (it would inflate the
        comparison because the prompt itself contains the answer).
    """
    DECISION = "decision"
    HANDOFF = "handoff"
    CORRECTION_SAFETY = "correction_safety"
    AUDITABILITY = "auditability"
    COST_LATENCY = "cost_latency"
    PROMPT_RETENTION = "prompt_retention"


class ScoreStatus(str, Enum):
    """How the row was scored.

    `scored`: deterministic scorer returned a real decision_score.
    `needs_judge`: the case has judge-only probes; deterministic
        scoring is not applicable. Such rows must carry
        decision_score = None and are excluded from deterministic
        aggregates (NEVER counted as zero).
    `skipped_context_too_large`: raw_oracle condition where the full
        event log doesn't fit. decision_score must be None.
    `errored`: the run failed; `notes` must explain.
    """
    SCORED = "scored"
    NEEDS_JUDGE = "needs_judge"
    SKIPPED_CONTEXT_TOO_LARGE = "skipped_context_too_large"
    ERRORED = "errored"


class AuditVerdict(str, Enum):
    """Mirrors the C++ AuditVerdict enum at the substrate boundary.

    The substrate values are kPass / kCorrectionEmitted /
    kInconclusive / kPending. We add `not_applicable` for non-DPM
    conditions so the column is present (and filterable) on every row
    instead of carrying nulls everywhere.
    """
    PASS = "pass"
    CORRECTION_EMITTED = "correction_emitted"
    INCONCLUSIVE = "inconclusive"
    PENDING = "pending"
    NOT_APPLICABLE = "not_applicable"


# ---- Errors ----------------------------------------------------------

class BenchRowError(ValueError):
    """Raised when a BenchRow violates the schema or a chart filter
    rejects a row that should not enter that chart."""


# ---- Row ----------------------------------------------------------------

@dataclass
class BenchRow:
    """One Phase 3 bench row.

    Required for every row:
      schema_version, run_id, case_id, corpus, condition, test_kind,
      budget_chars, repeat, model_id, score_status.

    DPM-specific (required when condition == DPM_PHASE3_CHECKPOINT and
    a checkpoint was actually used):
      checkpoint_manifest_hash, checkpoint_body_hash, event_range_start,
      event_range_end, log_generation, audit_certificate_id,
      audit_verdict, gate_may_use, blocking_corrections.

    If the gate refused to use any checkpoint (gate_may_use=False), the
    row is still valid as long as `gate_reason` is non-empty so a human
    can read why.
    """

    # --- identity / fairness columns
    schema_version: int = SCHEMA_VERSION
    run_id: str = ""
    case_id: str = ""
    corpus: str = ""              # "real_sessions" | "incident" | "agentic_qwen" | "synthetic"
    condition: Condition = Condition.ROLLING_SUMMARY
    test_kind: TestKind = TestKind.DECISION
    budget_chars: int = 0
    repeat: int = 0

    # --- model identity (every row, even non-DPM)
    model_id: str = ""
    projection_model_id: str = ""    # "" for non-DPM
    auditor_model_id: str = ""       # "" for non-DPM
    audit_policy_version: str = ""   # "" for non-DPM

    # --- event range (every row that consumed a log)
    event_count: int = 0
    event_range_start: int = 0
    event_range_end: int = 0

    # --- DPM substrate evidence (REQUIRED on DPM rows that used a
    # checkpoint; empty/null on non-DPM rows)
    checkpoint_manifest_hash: str = ""
    checkpoint_body_hash: str = ""
    log_generation: int = 0
    audit_certificate_id: str = ""
    audit_verdict: AuditVerdict = AuditVerdict.NOT_APPLICABLE
    drift_score: float | None = None
    blocking_corrections: list[str] = field(default_factory=list)
    gate_may_use: bool | None = None
    gate_reason: str = ""

    # --- scoring
    decision_score: float | None = None
    stale_memory_escape: bool | None = None  # null on rows where
                                              # the test isn't gauging
                                              # this axis
    audit_pass: bool | None = None            # null for non-DPM
    score_status: ScoreStatus = ScoreStatus.SCORED

    # --- cost / latency
    model_calls: int = 0
    input_tokens: int = 0
    output_tokens: int = 0
    wall_ms: int = 0

    # --- artifact hashes (audit / dedup)
    memory_sha256: str = ""
    answer_sha256: str = ""

    # --- free-form
    notes: str = ""

    def __post_init__(self) -> None:
        # Coerce string inputs (from JSONL load) to enums.
        if isinstance(self.condition, str):
            self.condition = Condition(self.condition)
        if isinstance(self.test_kind, str):
            self.test_kind = TestKind(self.test_kind)
        if isinstance(self.score_status, str):
            self.score_status = ScoreStatus(self.score_status)
        if isinstance(self.audit_verdict, str):
            self.audit_verdict = AuditVerdict(self.audit_verdict)
        validate_row(self)

    def to_dict(self) -> dict[str, Any]:
        d = asdict(self)
        for k in ("condition", "test_kind", "score_status", "audit_verdict"):
            v = d.get(k)
            d[k] = v.value if isinstance(v, Enum) else v
        return d

    def to_jsonl(self) -> str:
        return json.dumps(self.to_dict(), ensure_ascii=False, sort_keys=True)

    @classmethod
    def from_dict(cls, src: dict[str, Any]) -> "BenchRow":
        # Pick only known fields; ignore extras for forward-compat.
        known = {k: src[k] for k in src if k in cls.__dataclass_fields__}
        return cls(**known)


# ---- Validation ------------------------------------------------------

def _is_hex(s: str) -> bool:
    if not s:
        return False
    try:
        int(s, 16)
    except ValueError:
        return False
    return True


def validate_row(row: BenchRow) -> None:
    """Refuse rows that violate the schema, structurally."""

    # --- universal required fields
    if row.schema_version != SCHEMA_VERSION:
        raise BenchRowError(
            f"schema_version must be {SCHEMA_VERSION}, got {row.schema_version}")
    for required in ("run_id", "case_id", "corpus", "model_id"):
        if not getattr(row, required):
            raise BenchRowError(f"required field {required!r} is empty")
    if row.budget_chars <= 0:
        raise BenchRowError(
            f"budget_chars must be positive, got {row.budget_chars}")
    if row.repeat < 0:
        raise BenchRowError(f"repeat must be >= 0, got {row.repeat}")

    # --- score_status correctness
    if row.score_status == ScoreStatus.SCORED:
        if row.decision_score is None:
            raise BenchRowError(
                "score_status=scored requires decision_score to be set")
        if not (0.0 <= row.decision_score <= 1.0):
            raise BenchRowError(
                f"decision_score must be in [0, 1], got {row.decision_score}")
    elif row.score_status == ScoreStatus.NEEDS_JUDGE:
        if row.decision_score is not None:
            raise BenchRowError(
                "score_status=needs_judge MUST have decision_score=None "
                "(missing-judge rows are never counted as zero).")
    elif row.score_status == ScoreStatus.SKIPPED_CONTEXT_TOO_LARGE:
        if row.condition != Condition.RAW_ORACLE:
            raise BenchRowError(
                "skipped_context_too_large is only valid for "
                "condition=raw_oracle")
        if row.decision_score is not None:
            raise BenchRowError(
                "skipped rows must have decision_score=None")
    elif row.score_status == ScoreStatus.ERRORED:
        if not row.notes:
            raise BenchRowError(
                "score_status=errored requires non-empty notes "
                "(human-readable error explanation)")

    # --- DPM-condition substrate evidence
    if row.condition == Condition.DPM_PHASE3_CHECKPOINT:
        if not row.projection_model_id:
            raise BenchRowError(
                "DPM rows require projection_model_id")
        if not row.auditor_model_id:
            raise BenchRowError(
                "DPM rows require auditor_model_id")
        if not row.audit_policy_version:
            raise BenchRowError(
                "DPM rows require audit_policy_version")
        if row.audit_verdict == AuditVerdict.NOT_APPLICABLE:
            raise BenchRowError(
                "DPM rows must carry an audit_verdict other than "
                "not_applicable; use 'pending' if the audit hasn't run")
        if row.gate_may_use is None:
            raise BenchRowError(
                "DPM rows must record gate_may_use (true or false)")
        # Either the gate said yes (and we used a checkpoint), or it
        # said no — but the row must explain.
        if row.gate_may_use is True:
            if not row.checkpoint_manifest_hash:
                raise BenchRowError(
                    "DPM gate_may_use=True requires checkpoint_manifest_hash")
            if not row.checkpoint_body_hash:
                raise BenchRowError(
                    "DPM gate_may_use=True requires checkpoint_body_hash "
                    "(the gate accepted the projection; the row must "
                    "carry its content-addressed body)")
            if not row.audit_certificate_id:
                raise BenchRowError(
                    "DPM gate_may_use=True requires audit_certificate_id")
            if row.audit_pass is not True:
                raise BenchRowError(
                    "DPM gate_may_use=True requires audit_pass=True; "
                    "gate accept and audit pass are coupled")
            for h, name in (
                (row.checkpoint_manifest_hash, "checkpoint_manifest_hash"),
                (row.checkpoint_body_hash, "checkpoint_body_hash"),
                (row.audit_certificate_id, "audit_certificate_id"),
            ):
                if not _is_hex(h):
                    raise BenchRowError(
                        f"DPM {name} must be hex, got {h!r}")
        else:
            if not row.gate_reason:
                raise BenchRowError(
                    "DPM gate_may_use=False requires non-empty gate_reason")

    # --- non-DPM rows must NOT carry DPM evidence
    if row.condition != Condition.DPM_PHASE3_CHECKPOINT:
        if row.checkpoint_manifest_hash or row.audit_certificate_id:
            raise BenchRowError(
                f"condition={row.condition.value} cannot carry "
                "checkpoint_manifest_hash / audit_certificate_id; those "
                "are DPM-only fields")
        if row.audit_pass is not None:
            raise BenchRowError(
                f"condition={row.condition.value} must have audit_pass=None "
                "(rolling memory and raw oracle have no audit primitive; "
                "null is correct, NOT False)")
        if row.audit_verdict != AuditVerdict.NOT_APPLICABLE:
            raise BenchRowError(
                f"condition={row.condition.value} must have "
                "audit_verdict=not_applicable")

    # --- raw_oracle has at most one decision call
    if row.condition == Condition.RAW_ORACLE:
        if row.score_status == ScoreStatus.SCORED and row.model_calls != 1:
            raise BenchRowError(
                f"raw_oracle scored rows must have model_calls=1, "
                f"got {row.model_calls}")

    # --- range sanity
    if row.event_range_end < row.event_range_start:
        raise BenchRowError(
            f"event_range_end ({row.event_range_end}) < event_range_start "
            f"({row.event_range_start})")

    # --- drift_score domain
    if row.drift_score is not None:
        if not (0.0 <= row.drift_score <= 1.0):
            raise BenchRowError(
                f"drift_score must be in [0, 1] when set, got {row.drift_score}")


# ---- Chart guards ----------------------------------------------------

@dataclass(frozen=True)
class ChartSpec:
    """Declares which rows a chart is allowed to consume.

    The phase3-bench failure modes this prevents:

    1. Someone plots `prompt_retention` rows alongside `decision` rows,
       which makes both conditions look great because the prompt itself
       contains the answer.
    2. Someone plots rolling at budget=100 next to DPM at budget=200
       on the same case+test_kind cell. Quality is being compared
       across budgets, not within them. The fairness check enforces
       that within each (case_id, test_kind, repeat) cell, every row
       agrees on `budget_chars`.
    3. Cost/latency charts dropping needs_judge rows even though
       calls/tokens/wall_ms are still meaningful without a judge score.
       `excludes_needs_judge` is True for quality charts (the right
       call there) and False for cost charts.

    Calling `ChartSpec.filter` on a mixed list raises BenchRowError
    before any rendering happens.
    """
    name: str
    allowed_test_kinds: frozenset[TestKind]
    allowed_conditions: frozenset[Condition]
    excludes_needs_judge: bool = True

    def filter(self, rows: Iterable[BenchRow]) -> list[BenchRow]:
        out: list[BenchRow] = []
        for r in rows:
            if r.test_kind not in self.allowed_test_kinds:
                raise BenchRowError(
                    f"chart {self.name!r}: rejected test_kind="
                    f"{r.test_kind.value} (case {r.case_id}, "
                    f"condition {r.condition.value})")
            if r.condition not in self.allowed_conditions:
                raise BenchRowError(
                    f"chart {self.name!r}: rejected condition="
                    f"{r.condition.value} (case {r.case_id})")
            if (self.excludes_needs_judge and
                    r.score_status == ScoreStatus.NEEDS_JUDGE):
                # Excluded from deterministic quality charts. NEVER
                # counted as zero. Cost/latency keeps them.
                continue
            if r.budget_chars <= 0:
                raise BenchRowError(
                    f"chart {self.name!r}: rejected row without "
                    f"budget_chars (case {r.case_id})")
            out.append(r)

        # Per-cell budget consistency. A cell is the cross-condition
        # comparison unit: same case_id + test_kind + repeat. All rows
        # that survive the filter and share a cell must share a budget,
        # otherwise we're plotting rolling@100 next to DPM@200 and
        # calling that a comparison.
        cells: dict[tuple[str, str, int], dict[int, str]] = {}
        for r in out:
            key = (r.case_id, r.test_kind.value, r.repeat)
            seen = cells.setdefault(key, {})
            seen[r.budget_chars] = r.condition.value
        for (case_id, tk, rep), budget_to_cond in cells.items():
            if len(budget_to_cond) > 1:
                detail = ", ".join(
                    f"{cond}@{b}" for b, cond in
                    sorted(budget_to_cond.items()))
                raise BenchRowError(
                    f"chart {self.name!r}: cell "
                    f"(case_id={case_id!r}, test_kind={tk!r}, "
                    f"repeat={rep}) has rows at multiple budgets — "
                    f"{detail}. Charts compare conditions within a "
                    "budget, not across them.")
        return out


PHASE3_HANDOFF_QUALITY_CHART = ChartSpec(
    name="phase3_handoff_quality",
    allowed_test_kinds=frozenset({
        TestKind.DECISION,
        TestKind.HANDOFF,
        TestKind.CORRECTION_SAFETY,
    }),
    allowed_conditions=frozenset({
        Condition.RAW_ORACLE,
        Condition.ROLLING_SUMMARY,
        Condition.DPM_PHASE3_CHECKPOINT,
    }),
)

PHASE3_AUDIT_GATE_CHART = ChartSpec(
    name="phase3_audit_gate",
    allowed_test_kinds=frozenset({TestKind.AUDITABILITY}),
    allowed_conditions=frozenset({Condition.DPM_PHASE3_CHECKPOINT}),
)

PHASE3_COST_LATENCY_CHART = ChartSpec(
    name="phase3_cost_latency",
    allowed_test_kinds=frozenset({TestKind.COST_LATENCY}),
    allowed_conditions=frozenset({
        Condition.RAW_ORACLE,
        Condition.ROLLING_SUMMARY,
        Condition.DPM_PHASE3_CHECKPOINT,
    }),
    # Keep needs_judge rows. model_calls / input_tokens / output_tokens
    # / wall_ms are meaningful even when the answer can't be
    # deterministically scored.
    excludes_needs_judge=False,
)


def assert_quality_chart_rows(rows: Iterable[BenchRow]) -> list[BenchRow]:
    """Convenience wrapper around the headline quality chart spec.

    Equivalent to PHASE3_HANDOFF_QUALITY_CHART.filter(rows). Lifted to a
    standalone function because most callers from other lanes will use
    this one.
    """
    return PHASE3_HANDOFF_QUALITY_CHART.filter(rows)


# ---- Self-test -------------------------------------------------------

def _selftest() -> int:
    """Smoke. Run with `python bench_schema.py`."""
    fails = 0

    def expect_ok(builder, label):
        nonlocal fails
        try:
            r = builder()
            assert isinstance(r, BenchRow)
        except Exception as e:
            print(f"FAIL ok-case {label}: {type(e).__name__}: {e}")
            fails += 1

    def expect_err(builder, label):
        nonlocal fails
        try:
            builder()
        except BenchRowError:
            return
        except Exception as e:
            print(f"FAIL err-case {label}: wrong exception {type(e).__name__}: {e}")
            fails += 1
            return
        print(f"FAIL err-case {label}: expected BenchRowError, got OK")
        fails += 1

    # Valid raw_oracle row.
    expect_ok(lambda: BenchRow(
        run_id="2026-05-09-smoke",
        case_id="case-001", corpus="real_sessions",
        condition=Condition.RAW_ORACLE, test_kind=TestKind.DECISION,
        budget_chars=1338, repeat=0, model_id="claude-haiku-4-5",
        score_status=ScoreStatus.SCORED, decision_score=1.0,
        model_calls=1, input_tokens=42000, output_tokens=200,
        memory_sha256="0"*64, answer_sha256="1"*64,
    ), "valid raw_oracle scored")

    # Valid rolling_summary row.
    expect_ok(lambda: BenchRow(
        run_id="2026-05-09-smoke",
        case_id="case-001", corpus="real_sessions",
        condition=Condition.ROLLING_SUMMARY, test_kind=TestKind.DECISION,
        budget_chars=1338, repeat=0, model_id="claude-haiku-4-5",
        score_status=ScoreStatus.SCORED, decision_score=0.5,
        model_calls=17, input_tokens=85000, output_tokens=8000,
        memory_sha256="2"*64, answer_sha256="3"*64,
    ), "valid rolling_summary scored")

    # Valid DPM row with successful gate.
    expect_ok(lambda: BenchRow(
        run_id="2026-05-09-smoke",
        case_id="case-001", corpus="real_sessions",
        condition=Condition.DPM_PHASE3_CHECKPOINT,
        test_kind=TestKind.DECISION,
        budget_chars=1338, repeat=0, model_id="claude-haiku-4-5",
        projection_model_id="claude-haiku-4-5",
        auditor_model_id="dpm-exact-replay-auditor",
        audit_policy_version="exact-replay-v1",
        checkpoint_manifest_hash="ab"*32,
        checkpoint_body_hash="cd"*32,
        audit_certificate_id="ef"*32,
        audit_verdict=AuditVerdict.PASS,
        gate_may_use=True, audit_pass=True,
        drift_score=0.0,
        score_status=ScoreStatus.SCORED, decision_score=0.875,
        model_calls=2, input_tokens=78000, output_tokens=750,
        memory_sha256="4"*64, answer_sha256="5"*64,
    ), "valid DPM row gate=true")

    # Reject: DPM gate=true without checkpoint_body_hash
    expect_err(lambda: BenchRow(
        run_id="x", case_id="c", corpus="real_sessions",
        condition=Condition.DPM_PHASE3_CHECKPOINT, test_kind=TestKind.DECISION,
        budget_chars=100, model_id="m",
        projection_model_id="m", auditor_model_id="a",
        audit_policy_version="v1",
        checkpoint_manifest_hash="ab"*32,
        audit_certificate_id="ef"*32,
        audit_verdict=AuditVerdict.PASS,
        gate_may_use=True, audit_pass=True,
        score_status=ScoreStatus.SCORED, decision_score=1.0,
    ), "DPM gate=true missing checkpoint_body_hash")

    # Reject: DPM gate=true with audit_pass != True
    expect_err(lambda: BenchRow(
        run_id="x", case_id="c", corpus="real_sessions",
        condition=Condition.DPM_PHASE3_CHECKPOINT, test_kind=TestKind.DECISION,
        budget_chars=100, model_id="m",
        projection_model_id="m", auditor_model_id="a",
        audit_policy_version="v1",
        checkpoint_manifest_hash="ab"*32,
        checkpoint_body_hash="cd"*32,
        audit_certificate_id="ef"*32,
        audit_verdict=AuditVerdict.PASS,
        gate_may_use=True, audit_pass=None,
        score_status=ScoreStatus.SCORED, decision_score=1.0,
    ), "DPM gate=true with audit_pass=None")

    # Valid DPM row where gate refused.
    expect_ok(lambda: BenchRow(
        run_id="2026-05-09-smoke",
        case_id="case-002", corpus="real_sessions",
        condition=Condition.DPM_PHASE3_CHECKPOINT,
        test_kind=TestKind.CORRECTION_SAFETY,
        budget_chars=1338, repeat=0, model_id="claude-haiku-4-5",
        projection_model_id="claude-haiku-4-5",
        auditor_model_id="dpm-exact-replay-auditor",
        audit_policy_version="exact-replay-v1",
        audit_verdict=AuditVerdict.CORRECTION_EMITTED,
        drift_score=1.0,
        gate_may_use=False, audit_pass=False,
        gate_reason="blocking correction invalidates checkpoint ancestor",
        blocking_corrections=["corr-abc123"],
        score_status=ScoreStatus.SCORED, decision_score=1.0,
        model_calls=1, input_tokens=200, output_tokens=50,
        memory_sha256="6"*64, answer_sha256="7"*64,
    ), "valid DPM row gate=false")

    # Reject: needs_judge with decision_score set
    expect_err(lambda: BenchRow(
        run_id="x", case_id="c", corpus="real_sessions",
        condition=Condition.ROLLING_SUMMARY, test_kind=TestKind.DECISION,
        budget_chars=100, model_id="m",
        score_status=ScoreStatus.NEEDS_JUDGE, decision_score=0.0,
    ), "needs_judge with decision_score=0.0 (the trap)")

    # Reject: rolling_summary with audit_pass=False
    expect_err(lambda: BenchRow(
        run_id="x", case_id="c", corpus="real_sessions",
        condition=Condition.ROLLING_SUMMARY, test_kind=TestKind.DECISION,
        budget_chars=100, model_id="m",
        score_status=ScoreStatus.SCORED, decision_score=0.5,
        audit_pass=False,
    ), "rolling_summary with audit_pass=False (must be None)")

    # Reject: rolling_summary with checkpoint hash
    expect_err(lambda: BenchRow(
        run_id="x", case_id="c", corpus="real_sessions",
        condition=Condition.ROLLING_SUMMARY, test_kind=TestKind.DECISION,
        budget_chars=100, model_id="m",
        checkpoint_manifest_hash="ab"*32,
        score_status=ScoreStatus.SCORED, decision_score=0.5,
    ), "rolling_summary with checkpoint hash (DPM-only field)")

    # Reject: DPM gate_may_use=True without checkpoint hash
    expect_err(lambda: BenchRow(
        run_id="x", case_id="c", corpus="real_sessions",
        condition=Condition.DPM_PHASE3_CHECKPOINT, test_kind=TestKind.DECISION,
        budget_chars=100, model_id="m",
        projection_model_id="m", auditor_model_id="a",
        audit_policy_version="v1",
        audit_verdict=AuditVerdict.PASS,
        gate_may_use=True,
        score_status=ScoreStatus.SCORED, decision_score=1.0,
    ), "DPM gate=true without checkpoint hash")

    # Reject: DPM gate_may_use=False without gate_reason
    expect_err(lambda: BenchRow(
        run_id="x", case_id="c", corpus="real_sessions",
        condition=Condition.DPM_PHASE3_CHECKPOINT, test_kind=TestKind.DECISION,
        budget_chars=100, model_id="m",
        projection_model_id="m", auditor_model_id="a",
        audit_policy_version="v1",
        audit_verdict=AuditVerdict.CORRECTION_EMITTED,
        gate_may_use=False,
        score_status=ScoreStatus.SCORED, decision_score=1.0,
    ), "DPM gate=false without gate_reason")

    # Reject: raw_oracle scored with model_calls != 1
    expect_err(lambda: BenchRow(
        run_id="x", case_id="c", corpus="real_sessions",
        condition=Condition.RAW_ORACLE, test_kind=TestKind.DECISION,
        budget_chars=100, model_id="m",
        score_status=ScoreStatus.SCORED, decision_score=1.0,
        model_calls=3,
    ), "raw_oracle with model_calls=3")

    # Chart guard: reject prompt_retention row in quality chart
    pr_row = BenchRow(
        run_id="x", case_id="c", corpus="real_sessions",
        condition=Condition.ROLLING_SUMMARY,
        test_kind=TestKind.PROMPT_RETENTION,
        budget_chars=100, model_id="m",
        score_status=ScoreStatus.SCORED, decision_score=1.0,
    )
    try:
        assert_quality_chart_rows([pr_row])
        print("FAIL: prompt_retention row entered quality chart")
        fails += 1
    except BenchRowError:
        pass

    # Chart guard: needs_judge row excluded (not raised)
    nj_row = BenchRow(
        run_id="x", case_id="c", corpus="real_sessions",
        condition=Condition.ROLLING_SUMMARY, test_kind=TestKind.DECISION,
        budget_chars=100, model_id="m",
        score_status=ScoreStatus.NEEDS_JUDGE,
    )
    try:
        out = assert_quality_chart_rows([nj_row])
        if out:
            print(f"FAIL: needs_judge row not excluded; got {out}")
            fails += 1
    except BenchRowError as e:
        print(f"FAIL: needs_judge row raised instead of excluded: {e}")
        fails += 1

    # Chart guard: cost_latency chart KEEPS needs_judge rows
    nj_cost_row = BenchRow(
        run_id="x", case_id="c", corpus="real_sessions",
        condition=Condition.ROLLING_SUMMARY,
        test_kind=TestKind.COST_LATENCY,
        budget_chars=100, model_id="m",
        score_status=ScoreStatus.NEEDS_JUDGE,
        model_calls=17, input_tokens=80000, output_tokens=8000,
    )
    try:
        out = PHASE3_COST_LATENCY_CHART.filter([nj_cost_row])
        if not out:
            print("FAIL: cost chart dropped needs_judge row "
                  "(should keep — calls/tokens are still meaningful)")
            fails += 1
    except BenchRowError as e:
        print(f"FAIL: cost chart raised on needs_judge row: {e}")
        fails += 1

    # Chart guard: budget mismatch within a cell (rolling@100 + DPM@200
    # for the same case + test_kind) MUST raise.
    rolling_100 = BenchRow(
        run_id="x", case_id="case-X", corpus="real_sessions",
        condition=Condition.ROLLING_SUMMARY, test_kind=TestKind.DECISION,
        budget_chars=100, repeat=0, model_id="m",
        score_status=ScoreStatus.SCORED, decision_score=0.5,
    )
    dpm_200 = BenchRow(
        run_id="x", case_id="case-X", corpus="real_sessions",
        condition=Condition.DPM_PHASE3_CHECKPOINT,
        test_kind=TestKind.DECISION,
        budget_chars=200, repeat=0, model_id="m",
        projection_model_id="m", auditor_model_id="a",
        audit_policy_version="v1",
        checkpoint_manifest_hash="ab"*32,
        checkpoint_body_hash="cd"*32,
        audit_certificate_id="ef"*32,
        audit_verdict=AuditVerdict.PASS,
        gate_may_use=True, audit_pass=True,
        score_status=ScoreStatus.SCORED, decision_score=1.0,
    )
    try:
        assert_quality_chart_rows([rolling_100, dpm_200])
        print("FAIL: budget mismatch (100 vs 200) accepted by chart guard")
        fails += 1
    except BenchRowError:
        pass

    # Round-trip
    rt = BenchRow.from_dict(json.loads(BenchRow(
        run_id="x", case_id="c", corpus="real_sessions",
        condition=Condition.ROLLING_SUMMARY, test_kind=TestKind.DECISION,
        budget_chars=100, model_id="m",
        score_status=ScoreStatus.SCORED, decision_score=0.5,
    ).to_jsonl()))
    assert rt.condition == Condition.ROLLING_SUMMARY

    if fails:
        print(f"\nbench_schema._selftest: {fails} FAILURES")
    else:
        print("bench_schema._selftest: ALL PASS")
    return fails


if __name__ == "__main__":
    raise SystemExit(_selftest())
