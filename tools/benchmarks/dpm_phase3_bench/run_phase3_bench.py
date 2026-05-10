"""WP6 — matrix runner.

Loads SessionCase fixtures, expands the run matrix
(case x condition x budget x repeat x test_kind), invokes the agent
registry, scores each result, and writes one JSONL line per
(case, condition, budget, repeat, probe). Failed cases write an
errored row instead of crashing the whole run.

Agents come from `memory_agents.AGENT_REGISTRY` — a dict that engineers
3 and 4 populate. The runner fails loudly at startup if any expected
condition is missing.

Usage:

  python tools/benchmarks/dpm_phase3_bench/run_phase3_bench.py \\
    --fixtures tools/benchmarks/dpm_phase3_bench/fixtures/real_sessions \\
    --conditions raw_oracle,rolling_summary,dpm_phase3_checkpoint \\
    --budget_chars 1338,5352,21408 \\
    --repeat 1 \\
    --output .codex-local/phase3-bench-smoke/results.jsonl

Flags:

  --fixtures DIR        directory of SessionCase JSON fixtures (required)
  --conditions LIST     comma-separated subset of Condition values
                        (default: all three)
  --budget_chars LIST   comma-separated budget values
                        (default: 1338,5352,21408)
  --test_kinds LIST     comma-separated subset of TestKind values
                        (default: decision,handoff,correction_safety)
  --repeat N            number of repeat runs per cell (default: 1)
  --limit_cases N       run only the first N cases (default: all)
  --run_id ID           tag for the run (default: derived from output path)
  --output PATH         JSONL output (required, parent dirs auto-created)
  --dry_run             expand and print the matrix; do not call agents
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import sys
import time
import traceback
from dataclasses import asdict
from pathlib import Path
from typing import Any

try:
    from tools.benchmarks.dpm_phase3_bench.agent_protocol import (
        AgentResult, MemoryAgent,
    )
    from tools.benchmarks.dpm_phase3_bench.bench_schema import (
        BenchRow, Condition, TestKind, ScoreStatus, AuditVerdict,
        SCHEMA_VERSION, BenchRowError,
    )
    from tools.benchmarks.dpm_phase3_bench.score import score_probe, ScoreResult
    from tools.benchmarks.dpm_phase3_bench.session_case import (
        SessionCase, SessionProbe, iter_fixture_cases,
    )
except ModuleNotFoundError:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from agent_protocol import AgentResult, MemoryAgent  # type: ignore
    from bench_schema import (  # type: ignore
        BenchRow, Condition, TestKind, ScoreStatus, AuditVerdict,
        SCHEMA_VERSION, BenchRowError,
    )
    from score import score_probe, ScoreResult  # type: ignore
    from session_case import (  # type: ignore
        SessionCase, SessionProbe, iter_fixture_cases,
    )


_DEFAULT_BUDGETS = "1338,5352,21408"
_DEFAULT_TEST_KINDS = "decision,handoff,correction_safety"
_DEFAULT_CONDITIONS = "raw_oracle,rolling_summary,dpm_phase3_checkpoint"


# ---- Matrix expansion ------------------------------------------------

def _parse_csv(s: str) -> list[str]:
    return [x.strip() for x in s.split(",") if x.strip()]


def _resolve_conditions(raw: str) -> list[Condition]:
    out: list[Condition] = []
    for v in _parse_csv(raw):
        try:
            out.append(Condition(v))
        except ValueError:
            valid = ", ".join(c.value for c in Condition)
            raise SystemExit(
                f"--conditions: unknown value {v!r}; valid: {valid}")
    return out


_PROBE_KIND_TO_TEST_KIND = {
    "next_user_intent": TestKind.DECISION,
    "next_tool_call": TestKind.DECISION,
    "correction_detection": TestKind.CORRECTION_SAFETY,
    "handoff": TestKind.HANDOFF,
}


def _test_kind_for_probe(probe: SessionProbe) -> TestKind:
    """Derive a single TestKind for one cell from probe.kind.

    Replaces the old `_resolve_test_kinds` matrix axis. Falls back to
    DECISION for unrecognized probe kinds. See validity-fix series
    note at the --test_kinds CLI flag for rationale."""
    return _PROBE_KIND_TO_TEST_KIND.get(probe.kind, TestKind.DECISION)


def _resolve_budgets(raw: str) -> list[int]:
    out: list[int] = []
    for v in _parse_csv(raw):
        try:
            n = int(v)
        except ValueError:
            raise SystemExit(f"--budget_chars: not an int: {v!r}")
        if n <= 0:
            raise SystemExit(f"--budget_chars: must be positive, got {n}")
        out.append(n)
    return out


# ---- Agent loading ---------------------------------------------------

def _load_agent_registry(
    conditions: list[Condition],
) -> tuple[dict, "object | None"]:
    """Import memory_agents.AGENT_REGISTRY and a default ModelAdapter
    instance (used to instantiate agent factories that need a `model`
    arg).

    Returns (registry, default_model_adapter). Fails loudly if any
    requested condition is missing, so the runner never silently emits
    empty rows.
    """
    try:
        try:
            from tools.benchmarks.dpm_phase3_bench import memory_agents  # type: ignore
        except ModuleNotFoundError:
            import memory_agents  # type: ignore
    except ModuleNotFoundError as e:
        raise SystemExit(
            f"memory_agents module not found ({e}). "
            "Engineer 3 owns RawOracleAgent + RollingSummaryAgent; "
            "Engineer 4 owns DpmPhase3CheckpointAgent. "
            "memory_agents.py must export AGENT_REGISTRY."
        )

    registry = getattr(memory_agents, "AGENT_REGISTRY", None)
    if not isinstance(registry, dict):
        raise SystemExit(
            "memory_agents.AGENT_REGISTRY missing or not a dict.")

    missing = [c for c in conditions if c not in registry]
    if missing:
        names = ", ".join(c.value for c in missing)
        raise SystemExit(
            f"AGENT_REGISTRY missing condition(s): {names}. "
            "Either implement them or pass --conditions to skip.")

    # Default deterministic ModelAdapter for factories that take one.
    # If a real LLM-backed adapter is preferred, wrap the registry
    # entry in a lambda so the registry value is zero-arg.
    adapter_cls = getattr(memory_agents, "HeuristicModelAdapter", None)
    default_adapter = adapter_cls() if adapter_cls is not None else None
    return registry, default_adapter


def _instantiate_agent(factory, condition: Condition,
                        default_adapter) -> "MemoryAgent":
    """Instantiate a registry value. Convention is zero-arg, but
    Engineer 3's agents take a `model: ModelAdapter` arg; if a zero-arg
    call fails with a `model`-shaped TypeError and a default adapter is
    available, retry with it. Anything else surfaces verbatim."""
    try:
        return factory()
    except TypeError as e:
        msg = str(e)
        if "model" in msg and default_adapter is not None:
            return factory(default_adapter)
        raise SystemExit(
            f"AGENT_REGISTRY[{condition.value}] could not be instantiated: "
            f"{e}. Either wrap it in a zero-arg lambda or expose a "
            "compatible default adapter as memory_agents.HeuristicModelAdapter."
        )


# ---- Row construction ------------------------------------------------

def _sha256_hex(s: str) -> str:
    if not s:
        return ""
    return hashlib.sha256(s.encode("utf-8")).hexdigest()


def _row_from_result(
    *,
    run_id: str,
    case: SessionCase,
    probe: SessionProbe,
    condition: Condition,
    test_kind: TestKind,
    budget_chars: int,
    repeat: int,
    model_id: str,
    result: AgentResult,
    score: ScoreResult,
) -> BenchRow:
    # Hash artifacts the agent didn't pre-hash (cheap, deterministic).
    mem_hash = result.memory_sha256 or _sha256_hex(result.memory_bytes)
    ans_hash = result.answer_sha256 or _sha256_hex(result.answer_bytes)
    return BenchRow(
        schema_version=SCHEMA_VERSION,
        run_id=run_id,
        case_id=case.case_id,
        corpus=case.domain or "real_sessions",
        condition=condition,
        test_kind=test_kind,
        budget_chars=budget_chars,
        repeat=repeat,
        model_id=model_id,
        projection_model_id=result.projection_model_id,
        auditor_model_id=result.auditor_model_id,
        audit_policy_version=result.audit_policy_version,
        event_count=case.n_events,
        event_range_start=result.event_range_start,
        # Fall back to the case's full event log [0, n_events). probe_T
        # is the zero-based index of the probe target, so [0, probe_T)
        # under-claims by one event under half-open semantics. The
        # audit certificate's range describes which events the
        # substrate covered, not which events the agent saw before the
        # probe — agents that want a tighter range set
        # `result.event_range_end` themselves.
        event_range_end=result.event_range_end or case.n_events,
        log_generation=result.log_generation,
        checkpoint_manifest_hash=result.checkpoint_manifest_hash,
        checkpoint_body_hash=result.checkpoint_body_hash,
        audit_certificate_id=result.audit_certificate_id,
        audit_verdict=result.audit_verdict,
        drift_score=result.drift_score,
        blocking_corrections=list(result.blocking_corrections),
        gate_may_use=result.gate_may_use,
        gate_reason=result.gate_reason,
        decision_score=score.decision_score,
        stale_memory_escape=score.stale_memory_escape,
        audit_pass=score.audit_pass,
        score_status=score.score_status,
        model_calls=result.model_calls,
        input_tokens=result.input_tokens,
        output_tokens=result.output_tokens,
        wall_ms=result.wall_ms,
        memory_sha256=mem_hash,
        answer_sha256=ans_hash,
        notes=score.notes if score.notes else result.notes,
    )


def _errored_row(
    *,
    run_id: str,
    case: SessionCase,
    probe: SessionProbe,
    condition: Condition,
    test_kind: TestKind,
    budget_chars: int,
    repeat: int,
    model_id: str,
    error_message: str,
) -> BenchRow:
    """Best-effort row when the agent or scorer raised. Skipped rows
    don't count as zero in aggregates because score_status=errored is
    excluded from chart filters."""
    return BenchRow(
        schema_version=SCHEMA_VERSION,
        run_id=run_id,
        case_id=case.case_id,
        corpus=case.domain or "real_sessions",
        condition=condition,
        test_kind=test_kind,
        budget_chars=budget_chars,
        repeat=repeat,
        model_id=model_id,
        event_count=case.n_events,
        event_range_start=0,
        event_range_end=case.probe_T,
        score_status=ScoreStatus.ERRORED,
        decision_score=None,
        audit_verdict=(AuditVerdict.PENDING
                        if condition == Condition.DPM_PHASE3_CHECKPOINT
                        else AuditVerdict.NOT_APPLICABLE),
        gate_may_use=(False
                      if condition == Condition.DPM_PHASE3_CHECKPOINT
                      else None),
        gate_reason=(f"runner caught {error_message}"
                     if condition == Condition.DPM_PHASE3_CHECKPOINT
                     else ""),
        notes=f"errored: {error_message[:280]}",
    )


# ---- Cell execution --------------------------------------------------

def _run_cell(
    *,
    run_id: str,
    case: SessionCase,
    probe: SessionProbe,
    condition: Condition,
    test_kind: TestKind,
    budget_chars: int,
    repeat: int,
    model_id: str,
    agent: "MemoryAgent",
) -> BenchRow:
    """Run one (case, probe, condition, budget, repeat) cell. Catches
    agent-side and scorer-side exceptions and emits an errored row."""
    t0 = time.monotonic()
    try:
        result = agent.run(case, probe, budget_chars)
    except Exception as e:
        return _errored_row(
            run_id=run_id, case=case, probe=probe, condition=condition,
            test_kind=test_kind, budget_chars=budget_chars,
            repeat=repeat, model_id=model_id,
            error_message=f"agent.run: {type(e).__name__}: {e}",
        )

    # Stamp wall_ms if the agent didn't.
    if result.wall_ms == 0:
        result.wall_ms = int((time.monotonic() - t0) * 1000)

    try:
        score = score_probe(probe, result)
    except Exception as e:
        return _errored_row(
            run_id=run_id, case=case, probe=probe, condition=condition,
            test_kind=test_kind, budget_chars=budget_chars,
            repeat=repeat, model_id=model_id,
            error_message=f"score_probe: {type(e).__name__}: {e}\n"
                          f"{traceback.format_exc()[-280:]}",
        )

    try:
        return _row_from_result(
            run_id=run_id, case=case, probe=probe, condition=condition,
            test_kind=test_kind, budget_chars=budget_chars,
            repeat=repeat, model_id=model_id,
            result=result, score=score,
        )
    except BenchRowError as e:
        return _errored_row(
            run_id=run_id, case=case, probe=probe, condition=condition,
            test_kind=test_kind, budget_chars=budget_chars,
            repeat=repeat, model_id=model_id,
            error_message=f"BenchRow validation: {e}",
        )


# ---- Driver ----------------------------------------------------------

def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixtures", type=Path, required=True,
                    help="directory of SessionCase JSON fixtures")
    ap.add_argument("--conditions", default=_DEFAULT_CONDITIONS,
                    help="comma-separated Condition values")
    ap.add_argument("--budget_chars", default=_DEFAULT_BUDGETS,
                    help="comma-separated int budgets")
    # NOTE: --test_kinds removed from the matrix expansion in the
    # 2026-05 validity fix series. The previous matrix looped every
    # probe across decision/handoff/correction_safety, but the agent
    # was only ever called with (case, probe, budget); test_kind was
    # a bench-row label, not a distinct experiment. The 162-row
    # matrix in r3 was 54 distinct cells × 3 relabels. Now each cell
    # is one (case, probe, condition, budget, repeat); test_kind is
    # derived from probe.kind. The CLI flag is kept for backward
    # compat but ignored.
    ap.add_argument("--test_kinds", default=_DEFAULT_TEST_KINDS,
                    help="(IGNORED — kept for back-compat) comma-separated TestKind values")
    ap.add_argument("--repeat", type=int, default=1,
                    help="repeats per cell (default 1)")
    ap.add_argument("--limit_cases", type=int, default=0,
                    help="run only the first N cases (0 = all)")
    ap.add_argument("--run_id", default="",
                    help="tag for this run (default: timestamp)")
    ap.add_argument("--output", type=Path, required=True,
                    help="JSONL output path")
    ap.add_argument("--model_id", default="claude-opus-4-7",
                    help="model_id stamped on every row")
    ap.add_argument("--dry_run", action="store_true",
                    help="expand and print the matrix; do not call agents")
    args = ap.parse_args(argv)

    conditions = _resolve_conditions(args.conditions)
    budgets = _resolve_budgets(args.budget_chars)
    repeats = max(1, args.repeat)
    run_id = args.run_id or dt.datetime.now(dt.timezone.utc).strftime(
        "%Y-%m-%dT%H%M%SZ-phase3")

    if not args.fixtures.exists():
        raise SystemExit(f"fixtures directory not found: {args.fixtures}")
    cases = iter_fixture_cases(args.fixtures)
    if args.limit_cases:
        cases = cases[: args.limit_cases]
    if not cases:
        raise SystemExit(
            f"no SessionCase fixtures found under {args.fixtures}")

    # Expand cells. test_kind is derived per-probe from probe.kind,
    # NOT looped over: each (case, probe, condition, budget, repeat)
    # is a single distinct experiment. See note at the
    # ap.add_argument("--test_kinds", ...) call site.
    cells: list[tuple[Path, SessionCase, SessionProbe, Condition, TestKind,
                      int, int]] = []
    for path, case in cases:
        if not case.probes:
            print(f"  warn: case {case.case_id} has no probes; skipping",
                  file=sys.stderr)
            continue
        for probe in case.probes:
            tk = _test_kind_for_probe(probe)
            for cond in conditions:
                for budget in budgets:
                    for rep in range(repeats):
                        cells.append(
                            (path, case, probe, cond, tk, budget, rep))

    print(f"matrix: {len(cells)} cells "
          f"({len(cases)} cases x {len(conditions)} conditions x "
          f"{len(budgets)} budgets x {repeats} repeats; "
          f"test_kind derived per-probe), run_id={run_id}")
    if args.dry_run:
        for path, case, probe, cond, tk, budget, rep in cells[:25]:
            print(
                f"  {case.case_id} | {cond.value} | {tk.value} | "
                f"budget={budget} | repeat={rep} | probe={probe.kind}")
        if len(cells) > 25:
            print(f"  ... ({len(cells) - 25} more)")
        return 0

    registry, default_adapter = _load_agent_registry(conditions)
    # Instantiate agents once per condition (most are cheap; DPM may
    # hold open a CheckpointStore handle).
    agents: dict[Condition, MemoryAgent] = {
        cond: _instantiate_agent(registry[cond], cond, default_adapter)
        for cond in conditions
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    written = 0
    errored = 0
    with args.output.open("w", encoding="utf-8") as out_fp:
        for path, case, probe, cond, tk, budget, rep in cells:
            row = _run_cell(
                run_id=run_id, case=case, probe=probe, condition=cond,
                test_kind=tk, budget_chars=budget, repeat=rep,
                model_id=args.model_id, agent=agents[cond],
            )
            out_fp.write(row.to_jsonl() + "\n")
            out_fp.flush()
            written += 1
            if row.score_status == ScoreStatus.ERRORED:
                errored += 1
                print(f"  ERR {case.case_id} {cond.value} {tk.value} "
                      f"budget={budget}: {row.notes[:120]}", file=sys.stderr)

    print(f"wrote {written} rows ({errored} errored) -> {args.output}")
    return 0 if errored == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
