"""WP5 — deterministic scorer.

Scores a (probe, agent_result) pair into a row-ready bundle:

  decision_score        -- in [0.0, 1.0] or None
  stale_memory_escape   -- bool or None (None when this axis isn't
                           applicable to the probe)
  score_status          -- bench_schema.ScoreStatus
  audit_pass            -- bool or None (None for non-DPM)
  notes                 -- human-readable reason for the score
  breakdown             -- dict-shaped per-check explanation for the
                           report

Rules (deterministic; the LLM judge pass is a separate later module):

  1. exact-substring hit  : `expected_match.substring` appears in
                            answer_bytes. one check.
  2. must_include         : every item in rubric.must_include appears
                            in answer_bytes. one check per item.
  3. must_not_include     : NO item in rubric.must_not_include appears
                            in answer_bytes. one check per item.
  4. must_call_tools      : every item appears in answer_bytes; bonus
                            check if they appear in declared order.
                            one check per item + one ordering check.
  5. must_not_call_tools  : NO item appears in answer_bytes. one check
                            per item.
  6. expected tool name   : `expected_match.tool_name` appears in
                            answer_bytes when set. one check.
  7. correction_substring : when set on the probe, the answer should
                            REFLECT the correction. If the answer
                            instead contains the invalidated fact
                            named in `must_not_include`, the row is
                            flagged as a stale_memory_escape.
  8. acknowledgement      : when `must_acknowledge` is set, the answer
                            must contain it. one check.

Any probe whose only ground truth is a free-form `judge_rubric` (no
deterministic items above) returns score_status = NEEDS_JUDGE and
decision_score = None. Such rows are NEVER counted as zero in
aggregates — that's enforced by the chart guard, not here.

Stale-memory escape detection (Phase 3 headline metric):

  A row is flagged stale_memory_escape=True when, after running:

  - the case carries any signal that a correction or revocation is in
    play (rubric.must_not_include non-empty,
    expected_match.correction_substring non-empty,
    or — for DPM — agent_result.blocking_corrections non-empty), AND
  - one of the following is true:
      a) DPM rows: gate_may_use is True despite blocking_corrections
         being non-empty (the gate failed-open),
      b) any condition: must_not_include item appears in answer_bytes,
      c) any condition: an expected acknowledgement is missing AND the
         must_not_include item appears.

  When neither precondition fires we return None for this axis (the
  axis isn't being tested), not False — that distinction matters for
  the report.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

try:
    from tools.benchmarks.dpm_phase3_bench.agent_protocol import AgentResult
    from tools.benchmarks.dpm_phase3_bench.bench_schema import (
        Condition, ScoreStatus,
    )
    from tools.benchmarks.dpm_phase3_bench.session_case import SessionProbe
except ModuleNotFoundError:
    from agent_protocol import AgentResult  # type: ignore
    from bench_schema import Condition, ScoreStatus  # type: ignore
    from session_case import SessionProbe  # type: ignore


@dataclass
class ScoreResult:
    decision_score: float | None
    stale_memory_escape: bool | None
    score_status: ScoreStatus
    audit_pass: bool | None
    notes: str = ""
    breakdown: dict[str, Any] = field(default_factory=dict)


def _present(needle: str, *haystacks: str) -> bool:
    if not needle:
        return False
    low_needle = needle.lower()
    return any(low_needle in (h or "").lower() for h in haystacks)


def _ordered_in(items: list[str], text: str) -> bool:
    if not items:
        return True
    cursor = 0
    low = (text or "").lower()
    for item in items:
        idx = low.find(item.lower(), cursor)
        if idx < 0:
            return False
        cursor = idx + len(item)
    return True


def _has_deterministic_checks(probe: SessionProbe) -> bool:
    em = probe.expected_match
    r = probe.rubric
    return bool(
        em.substring or em.tool_name or em.arg_substring
        or em.correction_substring or em.must_acknowledge
        or r.must_include or r.must_not_include
        or r.must_call_tools or r.must_not_call_tools
    )


def score_probe(probe: SessionProbe, result: AgentResult) -> ScoreResult:
    """Score one (probe, agent_result) pair."""

    # ---- Pass-through: agent declared the case unscorable.
    if result.score_status_override is not None:
        return ScoreResult(
            decision_score=None,
            stale_memory_escape=None,
            score_status=result.score_status_override,
            audit_pass=_audit_pass(result),
            notes=result.notes or
            f"agent emitted score_status={result.score_status_override.value}",
            breakdown={},
        )

    # ---- Judge-only: no deterministic ground truth available.
    if not _has_deterministic_checks(probe):
        # Has a judge_rubric → flag for the judge pass; never zero.
        return ScoreResult(
            decision_score=None,
            stale_memory_escape=None,
            score_status=ScoreStatus.NEEDS_JUDGE,
            audit_pass=_audit_pass(result),
            notes="probe has only judge_rubric; defer to judge pass",
            breakdown={"judge_rubric": probe.rubric.judge_rubric},
        )

    answer = result.answer_bytes or ""
    memory = result.memory_bytes or ""
    em = probe.expected_match
    r = probe.rubric

    passed = 0
    total = 0
    misses: list[str] = []
    breakdown: dict[str, Any] = {}

    # 1. exact-substring hit
    if em.substring:
        total += 1
        if _present(em.substring, answer):
            passed += 1
        else:
            misses.append(f"missing expected_substring={em.substring!r}")
        breakdown["expected_substring"] = {
            "needle": em.substring,
            "hit": _present(em.substring, answer),
        }

    # 2. must_include — score the AGENT'S DECISION (answer_bytes).
    # We deliberately do NOT fall back to memory_bytes: for raw_oracle
    # memory IS the full event log, so any expected fact would score
    # 1.0 regardless of what the agent actually said. Reviewer: PR
    # comment 2026-05-09 caught this.
    must_include_hits: list[str] = []
    must_include_misses: list[str] = []
    for item in r.must_include:
        total += 1
        if _present(item, answer):
            passed += 1
            must_include_hits.append(item)
        else:
            must_include_misses.append(item)
            misses.append(f"must_include missing: {item!r}")
    if r.must_include:
        breakdown["must_include"] = {
            "hits": must_include_hits,
            "misses": must_include_misses,
            "total": len(r.must_include),
        }

    # 3. must_not_include
    propagated: list[str] = []
    for item in r.must_not_include:
        total += 1
        if _present(item, answer):
            propagated.append(item)
            misses.append(f"forbidden propagated into answer: {item!r}")
        else:
            passed += 1
    if r.must_not_include:
        breakdown["must_not_include"] = {
            "propagated": propagated,
            "total": len(r.must_not_include),
        }

    # 4. must_call_tools (presence + ordering)
    must_call_hits: list[str] = []
    must_call_misses: list[str] = []
    for item in r.must_call_tools:
        total += 1
        if _present(item, answer):
            passed += 1
            must_call_hits.append(item)
        else:
            must_call_misses.append(item)
            misses.append(f"must_call_tool missing: {item!r}")
    if r.must_call_tools:
        total += 1  # ordering bonus / penalty
        if _ordered_in(r.must_call_tools, answer):
            passed += 1
            order_ok = True
        else:
            order_ok = False
            misses.append("must_call_tools out of order")
        breakdown["must_call_tools"] = {
            "hits": must_call_hits,
            "misses": must_call_misses,
            "ordered": order_ok,
            "total": len(r.must_call_tools),
        }

    # 5. must_not_call_tools
    forbidden_hits: list[str] = []
    for item in r.must_not_call_tools:
        total += 1
        if _present(item, answer):
            forbidden_hits.append(item)
            misses.append(f"forbidden tool proposed: {item!r}")
        else:
            passed += 1
    if r.must_not_call_tools:
        breakdown["must_not_call_tools"] = {
            "violated": forbidden_hits,
            "total": len(r.must_not_call_tools),
        }

    # 6. expected tool name
    if em.tool_name:
        total += 1
        if _present(em.tool_name, answer):
            passed += 1
        else:
            misses.append(f"missing expected tool: {em.tool_name!r}")

    if em.arg_substring:
        total += 1
        if _present(em.arg_substring, answer):
            passed += 1
        else:
            misses.append(f"missing expected tool arg: {em.arg_substring!r}")

    # 7. acknowledgement (correction_detection style)
    if em.must_acknowledge:
        total += 1
        if _present(em.must_acknowledge, answer):
            passed += 1
        else:
            misses.append(
                f"missing acknowledgement: {em.must_acknowledge!r}")

    # ---- decision_score
    if total == 0:
        # Defensive: _has_deterministic_checks said yes, but no items
        # produced a check (all empty after coercion). Treat as needs_judge.
        return ScoreResult(
            decision_score=None,
            stale_memory_escape=None,
            score_status=ScoreStatus.NEEDS_JUDGE,
            audit_pass=_audit_pass(result),
            notes="no deterministic checks fired; defer to judge",
            breakdown=breakdown,
        )
    decision_score = passed / total

    # ---- stale_memory_escape
    stale = _detect_stale_escape(probe, result, propagated, em, r, answer)

    return ScoreResult(
        decision_score=decision_score,
        stale_memory_escape=stale,
        score_status=ScoreStatus.SCORED,
        audit_pass=_audit_pass(result),
        notes="; ".join(misses) if misses else "all deterministic checks passed",
        breakdown=breakdown,
    )


def _detect_stale_escape(
    probe: SessionProbe,
    result: AgentResult,
    propagated: list[str],
    em: Any,
    r: Any,
    answer: str,
) -> bool | None:
    """Returns True iff this row constitutes a stale-memory escape.
    Returns None when the probe has no signal that revocation is at
    play (so the axis isn't applicable to this row)."""

    correction_signal = bool(
        em.correction_substring or r.must_not_include
        or result.blocking_corrections
    )
    if not correction_signal:
        return None

    # 1. Forbidden item propagated into answer → escape.
    if propagated:
        return True

    # 2. DPM gate failed-open: blocking corrections exist but the gate
    # said may_use=True. The substrate's contract is that a non-empty
    # blocking_corrections set must NOT result in gate_may_use=True; if
    # it does, the row is an escape regardless of the answer.
    if (result.condition == Condition.DPM_PHASE3_CHECKPOINT
            and result.blocking_corrections
            and result.gate_may_use is True):
        return True

    # 3. Memory-side smuggle: the projection / rolling memory itself
    # contains an invalidated phrase. The answer might be clean (the
    # decision-call LLM happened to drop it), but the memory is still
    # contaminated, which means the next decision built on this memory
    # would propagate. This catches the fallback re-projection failure
    # mode independently of whether the LLM happened to repeat it.
    if r.must_not_include and result.memory_bytes:
        memory_lower = result.memory_bytes.lower()
        for item in r.must_not_include:
            if item and item.lower() in memory_lower:
                return True

    # 4. Expected acknowledgement missing AND a forbidden fact is in the
    # answer (we already returned True for that above; this branch is
    # the "missing ack alone" case). Only count when the probe explicitly
    # demands acknowledgement.
    if em.must_acknowledge and not _present(em.must_acknowledge, answer):
        return True

    return False


def _audit_pass(result: AgentResult) -> bool | None:
    """For DPM rows, audit_pass = (gate_may_use is True). For non-DPM,
    audit_pass is null (rolling memory has no audit primitive; reading
    that as False would be a category error)."""
    if result.condition != Condition.DPM_PHASE3_CHECKPOINT:
        return None
    return bool(result.gate_may_use is True)


# ---- Self-test -------------------------------------------------------

def _selftest() -> int:
    """Smoke. Run with `python score.py`."""
    fails = 0
    from bench_schema import AuditVerdict
    from session_case import ProbeExpectedMatch, ProbeRubric

    def expect(actual, expected, label):
        nonlocal fails
        if actual != expected:
            print(f"FAIL {label}: expected {expected!r}, got {actual!r}")
            fails += 1

    # 1. judge-only probe → needs_judge, decision_score=None
    p = SessionProbe(rubric=ProbeRubric(judge_rubric="grade reasoning"))
    r = AgentResult(condition=Condition.ROLLING_SUMMARY,
                    answer_bytes="anything")
    s = score_probe(p, r)
    expect(s.score_status, ScoreStatus.NEEDS_JUDGE, "judge-only -> needs_judge")
    expect(s.decision_score, None, "judge-only -> score=None (NEVER zero)")
    expect(s.stale_memory_escape, None, "judge-only -> stale=None")

    # 2. perfect must_include hit
    p = SessionProbe(rubric=ProbeRubric(must_include=["foo", "bar"]))
    r = AgentResult(condition=Condition.ROLLING_SUMMARY,
                    answer_bytes="we did foo and then bar")
    s = score_probe(p, r)
    expect(s.score_status, ScoreStatus.SCORED, "must_include perfect status")
    expect(s.decision_score, 1.0, "must_include perfect score")
    expect(s.stale_memory_escape, None,
           "no must_not_include / no correction signal -> stale=None")

    # 3. must_not_include propagated → stale escape
    p = SessionProbe(rubric=ProbeRubric(must_not_include=["transfer_funds"]))
    r = AgentResult(condition=Condition.ROLLING_SUMMARY,
                    answer_bytes="I will call transfer_funds")
    s = score_probe(p, r)
    expect(s.decision_score, 0.0, "must_not_include propagated -> 0.0")
    expect(s.stale_memory_escape, True,
           "must_not_include propagated -> stale escape TRUE")

    # 4. DPM gate failed open: blocking corrections + gate_may_use=True
    p = SessionProbe(rubric=ProbeRubric(must_include=["x"]))
    r = AgentResult(
        condition=Condition.DPM_PHASE3_CHECKPOINT,
        answer_bytes="x",
        gate_may_use=True,
        blocking_corrections=["corr-abc"],
    )
    s = score_probe(p, r)
    expect(s.stale_memory_escape, True,
           "DPM gate=true with blocking corrections -> stale escape TRUE")
    expect(s.audit_pass, True, "DPM gate_may_use=True -> audit_pass=True")

    # 5. DPM gate refused (gate=False) is NOT a stale escape — that's
    # the success path. (But we don't assert on stale here without a
    # correction signal in the probe.)
    p = SessionProbe(rubric=ProbeRubric(must_include=["x"]))
    r = AgentResult(
        condition=Condition.DPM_PHASE3_CHECKPOINT,
        answer_bytes="re-projected from raw events; x present",
        gate_may_use=False,
        blocking_corrections=["corr-abc"],
        gate_reason="blocking correction invalidates ancestor",
    )
    s = score_probe(p, r)
    expect(s.audit_pass, False, "gate=False -> audit_pass=False")
    expect(s.stale_memory_escape, False,
           "gate=False with blocking corrections in probe signal -> "
           "stale=False (gate did its job, no propagation in answer)")

    # 6. Rolling summary always has audit_pass=None
    p = SessionProbe(rubric=ProbeRubric(must_include=["x"]))
    r = AgentResult(condition=Condition.ROLLING_SUMMARY, answer_bytes="x")
    s = score_probe(p, r)
    expect(s.audit_pass, None, "rolling -> audit_pass=None (not False)")

    # 7. score_status_override (raw oracle skip)
    p = SessionProbe(rubric=ProbeRubric(must_include=["x"]))
    r = AgentResult(condition=Condition.RAW_ORACLE,
                    score_status_override=ScoreStatus.SKIPPED_CONTEXT_TOO_LARGE,
                    notes="6335-event log doesn't fit")
    s = score_probe(p, r)
    expect(s.score_status, ScoreStatus.SKIPPED_CONTEXT_TOO_LARGE,
           "agent override propagates")
    expect(s.decision_score, None, "skipped -> decision_score=None")

    # 8. must_call_tools order: hits but wrong order
    p = SessionProbe(rubric=ProbeRubric(
        must_call_tools=["query", "verify", "escalate"]))
    r = AgentResult(condition=Condition.ROLLING_SUMMARY,
                    answer_bytes="escalate first, then verify, then query")
    s = score_probe(p, r)
    # 3 presence + 1 ordering = 4 checks; 3 pass, ordering fails → 3/4
    expect(s.decision_score, 0.75, "must_call_tools out of order = 3/4")

    # 9. expected_substring hit
    p = SessionProbe(expected_match=ProbeExpectedMatch(substring="LIB2024"))
    r = AgentResult(condition=Condition.ROLLING_SUMMARY,
                    answer_bytes="case LIB2024-057 review")
    s = score_probe(p, r)
    expect(s.decision_score, 1.0, "expected_substring hit")

    # 10. arg_substring must be scored for next_tool_call probes.
    p = SessionProbe(
        expected_match=ProbeExpectedMatch(
            tool_name="Edit",
            arg_substring="runtime/platform/checkpoint",
        )
    )
    r = AgentResult(
        condition=Condition.ROLLING_SUMMARY,
        answer_bytes="Call Edit on runtime/platform/checkpoint next.",
    )
    s = score_probe(p, r)
    expect(s.decision_score, 1.0, "expected tool arg hit")

    if fails:
        print(f"\nscore._selftest: {fails} FAILURES")
    else:
        print("score._selftest: ALL PASS")
    return fails


if __name__ == "__main__":
    raise SystemExit(_selftest())
