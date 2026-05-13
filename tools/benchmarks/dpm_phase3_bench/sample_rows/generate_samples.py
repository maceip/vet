"""Emit one valid sample row per condition.

Engineer 4 (report/charts) consumes these to start the report lane in
parallel with the actual runner. The shape these rows take is the only
contract the report lane should depend on; everything else is plumbing.

Six rows in total:
  1. raw_oracle, decision, scored
  2. raw_oracle, decision, skipped_context_too_large
  3. rolling_summary, decision, scored (the "stale escape" case)
  4. rolling_summary, correction_safety, scored (escaped)
  5. dpm_phase3_checkpoint, decision, scored, gate=true
  6. dpm_phase3_checkpoint, correction_safety, scored, gate=false (refused)

Run:  python sample_rows/generate_samples.py > sample_rows.jsonl
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from bench_schema import (  # noqa: E402
    BenchRow, Condition, TestKind, ScoreStatus, AuditVerdict,
)

RUN_ID = "2026-05-09-phase3-sample"
CASE_ID = "real-session-correction-heavy-001"
CORPUS = "real_sessions"
MODEL = "claude-haiku-4-5-20251001"
BUDGET = 1338


def rows() -> list[BenchRow]:
    return [
        # 1. Raw oracle, full log fits, scored
        BenchRow(
            run_id=RUN_ID, case_id=CASE_ID, corpus=CORPUS,
            condition=Condition.RAW_ORACLE, test_kind=TestKind.DECISION,
            budget_chars=BUDGET, repeat=0, model_id=MODEL,
            event_count=128, event_range_start=0, event_range_end=128,
            score_status=ScoreStatus.SCORED, decision_score=1.0,
            stale_memory_escape=False,
            model_calls=1, input_tokens=42000, output_tokens=200, wall_ms=4200,
            memory_sha256="0" * 64, answer_sha256="a" * 64,
            notes="raw oracle ceiling",
        ),

        # 2. Raw oracle, log too large, skipped (NOT counted as zero)
        BenchRow(
            run_id=RUN_ID, case_id="real-session-long-001", corpus=CORPUS,
            condition=Condition.RAW_ORACLE, test_kind=TestKind.DECISION,
            budget_chars=BUDGET, repeat=0, model_id=MODEL,
            event_count=6335, event_range_start=0, event_range_end=6335,
            score_status=ScoreStatus.SKIPPED_CONTEXT_TOO_LARGE,
            decision_score=None,
            model_calls=0, input_tokens=0, output_tokens=0, wall_ms=0,
            notes="log of 6335 events exceeds single-call input budget",
        ),

        # 3. Rolling summary, decision, scored. The "stale plausible memory" row.
        BenchRow(
            run_id=RUN_ID, case_id=CASE_ID, corpus=CORPUS,
            condition=Condition.ROLLING_SUMMARY, test_kind=TestKind.DECISION,
            budget_chars=BUDGET, repeat=0, model_id=MODEL,
            event_count=128, event_range_start=0, event_range_end=128,
            score_status=ScoreStatus.SCORED, decision_score=0.375,
            stale_memory_escape=True,
            model_calls=17, input_tokens=85661, output_tokens=8035, wall_ms=64200,
            memory_sha256="b" * 64, answer_sha256="c" * 64,
            notes="rolling memory drifted; final summary lost the user's "
                  "first instruction",
        ),

        # 4. Rolling summary, correction_safety: did NOT refuse stale memory.
        BenchRow(
            run_id=RUN_ID, case_id=CASE_ID, corpus=CORPUS,
            condition=Condition.ROLLING_SUMMARY,
            test_kind=TestKind.CORRECTION_SAFETY,
            budget_chars=BUDGET, repeat=0, model_id=MODEL,
            event_count=128, event_range_start=0, event_range_end=128,
            score_status=ScoreStatus.SCORED, decision_score=0.0,
            stale_memory_escape=True,
            model_calls=17, input_tokens=85661, output_tokens=8035, wall_ms=64200,
            memory_sha256="b" * 64, answer_sha256="c" * 64,
            notes="rolling memory has no revocation primitive; stale "
                  "fact propagated into the answer",
        ),

        # 5. DPM phase3, gate accepted, scored.
        BenchRow(
            run_id=RUN_ID, case_id=CASE_ID, corpus=CORPUS,
            condition=Condition.DPM_PHASE3_CHECKPOINT,
            test_kind=TestKind.DECISION,
            budget_chars=BUDGET, repeat=0, model_id=MODEL,
            projection_model_id=MODEL,
            auditor_model_id="dpm-exact-replay-auditor",
            audit_policy_version="exact-replay-v1",
            event_count=128, event_range_start=0, event_range_end=128,
            log_generation=128,
            checkpoint_manifest_hash="ab"*32,
            checkpoint_body_hash="cd"*32,
            audit_certificate_id="ef"*32,
            audit_verdict=AuditVerdict.PASS,
            drift_score=0.0,
            gate_may_use=True, audit_pass=True,
            score_status=ScoreStatus.SCORED, decision_score=0.875,
            stale_memory_escape=False,
            model_calls=2, input_tokens=78252, output_tokens=750, wall_ms=7500,
            memory_sha256="d" * 64, answer_sha256="e" * 64,
            notes="DPM rebuild + audit pass + gate accept",
        ),

        # 6. DPM phase3, correction emitted, gate REFUSED. The headline win.
        BenchRow(
            run_id=RUN_ID, case_id=CASE_ID, corpus=CORPUS,
            condition=Condition.DPM_PHASE3_CHECKPOINT,
            test_kind=TestKind.CORRECTION_SAFETY,
            budget_chars=BUDGET, repeat=0, model_id=MODEL,
            projection_model_id=MODEL,
            auditor_model_id="dpm-exact-replay-auditor",
            audit_policy_version="exact-replay-v1",
            event_count=128, event_range_start=0, event_range_end=128,
            log_generation=128,
            audit_verdict=AuditVerdict.CORRECTION_EMITTED,
            drift_score=1.0,
            gate_may_use=False, audit_pass=False,
            gate_reason="blocking correction invalidates checkpoint "
                        "(corr-9b291a18 targets manifest ancestor)",
            blocking_corrections=["corr-9b291a18"],
            score_status=ScoreStatus.SCORED, decision_score=1.0,
            stale_memory_escape=False,
            model_calls=1, input_tokens=200, output_tokens=64, wall_ms=180,
            memory_sha256="", answer_sha256="f" * 64,
            notes="DPM gate refused stale checkpoint; agent re-projected "
                  "from raw events for the next decision",
        ),
    ]


if __name__ == "__main__":
    for r in rows():
        print(r.to_jsonl())
