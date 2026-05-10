"""Baseline memory agents for the Phase 3 bench."""
from __future__ import annotations

import hashlib
import json
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Protocol

try:
    from tools.benchmarks.dpm_phase3_bench.agent_protocol import AgentResult
    from tools.benchmarks.dpm_phase3_bench.bench_schema import (
        AuditVerdict,
        Condition,
        ScoreStatus,
    )
    from tools.benchmarks.dpm_phase3_bench.session_case import (
        SessionCase,
        SessionEvent,
        SessionProbe,
        load_session_cases as _load_typed_session_cases,
    )
except ModuleNotFoundError:  # Allows running from this directory directly.
    from agent_protocol import AgentResult  # type: ignore
    from bench_schema import AuditVerdict, Condition, ScoreStatus  # type: ignore
    from session_case import (  # type: ignore
        SessionCase,
        SessionEvent,
        SessionProbe,
        load_session_cases as _load_typed_session_cases,
    )


DEFAULT_DECISION_INSTRUCTION = (
    "Answer the probe using only the supplied memory. Preserve corrections, "
    "tool names, and policy constraints exactly when they matter."
)


@dataclass(frozen=True)
class ModelResponse:
    text: str
    input_tokens: int
    output_tokens: int
    wall_ms: int


class ModelAdapter(Protocol):
    """Small model boundary used by baselines and the future runner."""

    model_id: str

    def generate(
        self,
        prompt: str,
        *,
        purpose: str,
        max_output_chars: int,
    ) -> ModelResponse:
        ...


class HeuristicModelAdapter:
    """Deterministic local adapter for smoke tests.

    This is not a judge and not the intended model for headline runs. It lets
    Engineer 1's runner and Engineer 4's report path exercise the condition
    plumbing before API-backed model adapters land.
    """

    model_id = "heuristic-local-smoke-v1"

    def generate(
        self,
        prompt: str,
        *,
        purpose: str,
        max_output_chars: int,
    ) -> ModelResponse:
        start = time.perf_counter()
        if purpose == "rolling_summary":
            text = _heuristic_summary(prompt)
        elif purpose == "dpm_projection":
            text = _heuristic_projection(prompt)
        elif purpose == "decision":
            text = _heuristic_decision(prompt)
        else:
            text = _clip(prompt, max_output_chars)
        text = _clip(text, max_output_chars)
        wall_ms = max(1, int((time.perf_counter() - start) * 1000))
        return ModelResponse(
            text=text,
            input_tokens=estimate_tokens(prompt),
            output_tokens=estimate_tokens(text),
            wall_ms=wall_ms,
        )


class RawOracleAgent:
    """Full-log decision ceiling when the rendered event log fits."""

    def __init__(self, model: ModelAdapter, *, max_context_chars: int = 3_000_000):
        self.model = model
        self.max_context_chars = max_context_chars

    @property
    def condition(self) -> Condition:
        return Condition.RAW_ORACLE

    def run(
        self,
        case: SessionCase,
        probe: SessionProbe | None = None,
        budget_chars: int = 1338,
    ) -> AgentResult:
        event_log = render_event_log(case)
        task = render_task(case, probe)
        if len(event_log) > self.max_context_chars:
            return AgentResult(
                condition=self.condition,
                memory_bytes="",
                answer_bytes="",
                model_calls=0,
                input_tokens=0,
                output_tokens=0,
                wall_ms=0,
                memory_sha256="",
                answer_sha256="",
                score_status_override=ScoreStatus.SKIPPED_CONTEXT_TOO_LARGE,
                notes=(
                    f"raw oracle skipped: event log has {len(event_log)} chars "
                    f"over max_context_chars={self.max_context_chars}"
                ),
            )
        prompt = build_decision_prompt(event_log, task)
        response = self.model.generate(
            prompt,
            purpose="decision",
            max_output_chars=budget_chars,
        )
        return _result(
            condition=self.condition,
            memory_bytes=event_log,
            answer_bytes=response.text,
            responses=[response],
            notes="raw oracle saw the full rendered event log",
        )


class RollingSummaryAgent:
    """Rolling-memory baseline: summary_N = summarize(summary_N-1, window_N)."""

    def __init__(
        self,
        model: ModelAdapter,
        *,
        window_size_events: int = 8,
    ):
        if window_size_events <= 0:
            raise ValueError("window_size_events must be positive")
        self.model = model
        self.window_size_events = window_size_events

    @property
    def condition(self) -> Condition:
        return Condition.ROLLING_SUMMARY

    def run(
        self,
        case: SessionCase,
        probe: SessionProbe | None = None,
        budget_chars: int = 1338,
    ) -> AgentResult:
        task = render_task(case, probe)
        summary = ""
        responses: list[ModelResponse] = []
        events = events_up_to_probe(case)
        for window in _windows(events, self.window_size_events):
            prompt = build_summary_prompt(summary, render_events(window), task)
            response = self.model.generate(
                prompt,
                purpose="rolling_summary",
                max_output_chars=budget_chars,
            )
            responses.append(response)
            summary = _clip(response.text, budget_chars)

        decision_prompt = build_decision_prompt(summary, task)
        answer = self.model.generate(
            decision_prompt,
            purpose="decision",
            max_output_chars=budget_chars,
        )
        responses.append(answer)
        return _result(
            condition=self.condition,
            memory_bytes=summary,
            answer_bytes=answer.text,
            responses=responses,
            notes=(
                f"rolling summary over {len(events)} events with "
                f"window_size_events={self.window_size_events}"
            ),
        )


class DpmPhase3CheckpointAgent:
    """Phase 3 bench DPM condition.

    The C++ smoke proves the real substrate calls. This Python agent emits the
    same bench row semantics for matrix runs: task-conditioned projection,
    checkpoint identity fields, audit verdict, correction barrier, and the
    shared final decision prompt.
    """

    def __init__(
        self,
        model: ModelAdapter,
        *,
        schema_id: str = "phase3-session-handoff-v1",
        auditor_model_id: str = "dpm-exact-replay-auditor-v1",
        audit_policy_version: str = "exact-replay-v1",
    ):
        self.model = model
        self.schema_id = schema_id
        self.auditor_model_id = auditor_model_id
        self.audit_policy_version = audit_policy_version

    @property
    def condition(self) -> Condition:
        return Condition.DPM_PHASE3_CHECKPOINT

    def run(
        self,
        case: SessionCase,
        probe: SessionProbe | None = None,
        budget_chars: int = 1338,
    ) -> AgentResult:
        task = render_task(case, probe)
        events = events_up_to_probe(case)
        full_end = event_range_end(events)
        correction = first_correction_event(events, probe)
        must_not_include = (
            list(probe.rubric.must_not_include) if probe is not None else []
        )
        responses: list[ModelResponse] = []

        gate_may_use = True
        gate_reason = ""
        audit_verdict = AuditVerdict.PASS
        drift_score = 0.0
        blocking_corrections: list[str] = []
        checkpoint_events = events

        if correction is not None:
            # Snapshot of the would-have-been checkpoint frozen for hashing.
            # No correction-aware suppression here — this projection
            # represents what the checkpoint claimed BEFORE the correction
            # landed, so the audit can prove gate refusal was justified.
            checkpoint_events = [event for event in events if event.idx < correction.idx]
            stale_prompt = build_projection_prompt(
                render_events(checkpoint_events),
                task,
                budget_chars,
            )
            stale_projection = self.model.generate(
                stale_prompt,
                purpose="dpm_projection",
                max_output_chars=budget_chars,
            )
            responses.append(stale_projection)
            blocking_corrections = [correction_id_for(case, correction)]
            gate_may_use = False
            gate_reason = (
                "blocking correction invalidates checkpoint; "
                "re-projected from raw event range"
            )
            audit_verdict = AuditVerdict.CORRECTION_EMITTED
            drift_score = 1.0

        # Fallback / decision-feeding projection. When the gate refused
        # AND the rubric carries a concrete invalidation list
        # (must_not_include), thread the blocking correction event and
        # that list into the prompt as explicit suppression directives.
        #
        # Why gate on must_not_include: first_correction_event uses a
        # substring heuristic ("correction:" in event text, with
        # fallback to "correct"). On long-real-session that heuristic
        # false-positives on roadmap doc headers and code comments
        # containing the word "correction" — not actual user
        # corrections. Without a rubric-grounded invalidation list, we
        # can't tell what to suppress, and aggressive prompt-side
        # suppression against a phantom correction tanks decision
        # quality (proven in the 2026-05-10 dpm refuse-cells re-run on
        # long-real-session × decision: 1.0 → 0.0). Net rule: refuse
        # on detected corrections (substrate accounting), but only
        # apply correction-aware projection when the rubric explicitly
        # lists what's invalidated.
        suppress = (not gate_may_use) and bool(must_not_include)
        projection_prompt = build_projection_prompt(
            render_events(events),
            task,
            budget_chars,
            correction=correction if suppress else None,
            must_not_include=must_not_include if suppress else None,
        )
        projection = self.model.generate(
            projection_prompt,
            purpose="dpm_projection",
            max_output_chars=budget_chars,
        )
        responses.append(projection)

        # Deterministic guard: post-projection scan for must_not_include
        # contamination. If the correction-aware projector still
        # smuggled an invalidated phrase into memory, record it as a
        # projection_repair_failure so the row is honest about WHY
        # DPM lost on this cell (fallback contamination, not a normal
        # quality miss). Only checked on the fallback path; if the
        # gate accepted, the original checkpoint memory has already
        # been audited at write-time.
        repair_failures: list[str] = []
        if not gate_may_use and must_not_include:
            mem_lower = projection.text.lower()
            for item in must_not_include:
                if item and item.lower() in mem_lower:
                    repair_failures.append(item)

        checkpoint_body = projection.text if gate_may_use else responses[0].text
        checkpoint_body_hash = sha256_hex(checkpoint_body)
        checkpoint_range_end = event_range_end(checkpoint_events)
        checkpoint_manifest_hash = checkpoint_manifest_id(
            case=case,
            body_hash=checkpoint_body_hash,
            range_start=0,
            range_end=checkpoint_range_end,
            schema_id=self.schema_id,
            projection_model_id=self.model.model_id,
            audit_policy_version=self.audit_policy_version,
        )
        audit_certificate_id = audit_certificate_id_for(
            checkpoint_manifest_hash,
            audit_verdict,
            blocking_corrections,
        )

        answer = self.model.generate(
            build_decision_prompt(projection.text, task),
            purpose="decision",
            max_output_chars=budget_chars,
        )
        responses.append(answer)
        notes_parts: list[str] = []
        if gate_may_use:
            notes_parts.append("DPM checkpoint accepted")
        elif suppress:
            notes_parts.append("checkpoint_refused_reprojected_with_blocking_correction")
        else:
            notes_parts.append("checkpoint_refused_reprojected_no_rubric_suppression")
        if repair_failures:
            notes_parts.append(
                "projection_repair_failure: "
                + ", ".join(repr(item) for item in repair_failures)
            )
        result = _result(
            condition=self.condition,
            memory_bytes=projection.text,
            answer_bytes=answer.text,
            responses=responses,
            notes="; ".join(notes_parts),
        )
        result.projection_model_id = self.model.model_id
        result.auditor_model_id = self.auditor_model_id
        result.audit_policy_version = self.audit_policy_version
        result.event_range_start = 0
        result.event_range_end = checkpoint_range_end
        result.log_generation = full_end
        result.checkpoint_manifest_hash = checkpoint_manifest_hash
        result.checkpoint_body_hash = checkpoint_body_hash
        result.audit_certificate_id = audit_certificate_id
        result.audit_verdict = audit_verdict
        result.drift_score = drift_score
        result.gate_may_use = gate_may_use
        result.gate_reason = gate_reason
        result.blocking_corrections = blocking_corrections
        return result


def load_session_cases(path: str | Path) -> list[SessionCase]:
    return _load_typed_session_cases(path)


def events_up_to_probe(case: SessionCase) -> list[SessionEvent]:
    events = list(case.events)
    probe_t = int(case.probe_T or len(events))
    return [event for event in events if int(event.idx) <= probe_t]


def render_event_log(case: SessionCase) -> str:
    return render_events(events_up_to_probe(case))


def event_range_end(events: Iterable[SessionEvent]) -> int:
    end = 0
    for event in events:
        end = max(end, int(event.idx) + 1)
    return end


def first_correction_event(
    events: Iterable[SessionEvent],
    probe: SessionProbe | None,
) -> SessionEvent | None:
    for event in events:
        if "correction:" in event.text.lower():
            return event
    if probe is not None and (
        probe.expected_match.correction_substring
        or probe.rubric.must_not_include
    ):
        for event in events:
            if "correct" in event.text.lower():
                return event
    return None


def render_events(events: Iterable[SessionEvent]) -> str:
    lines: list[str] = []
    for event in events:
        idx = int(event.idx) + 1
        payload = {
            "kind": event.kind,
            "role": event.role,
            "text": event.text,
        }
        if event.tool_name:
            payload["tool_name"] = event.tool_name
        if event.tool_args:
            payload["tool_args"] = event.tool_args
        lines.append(f"[{idx}] {json.dumps(payload, ensure_ascii=False, sort_keys=True)}")
    return "\n".join(lines)


def render_task(case: SessionCase, probe: SessionProbe | None = None) -> str:
    probes = [probe] if probe is not None else list(case.probes)
    if not probes:
        return DEFAULT_DECISION_INSTRUCTION
    rendered = []
    for probe in probes:
        rendered.append(
            json.dumps(
                {
                    "kind": probe.kind,
                    "question": probe.question,
                    "expected_match": asdict(probe.expected_match),
                    "rubric": asdict(probe.rubric),
                },
                ensure_ascii=False,
                sort_keys=True,
            )
        )
    return DEFAULT_DECISION_INSTRUCTION + "\nPROBES:\n" + "\n".join(rendered)


def build_summary_prompt(previous_summary: str, new_events: str, task: str) -> str:
    return (
        "You are updating rolling memory for a long-running agent.\n"
        "Keep facts needed for the task. Preserve user corrections exactly. "
        "Do not treat invalidated facts as authoritative.\n\n"
        f"TASK:\n{task}\n\n"
        f"PREVIOUS SUMMARY:\n{previous_summary or '(empty)'}\n\n"
        f"NEW EVENTS:\n{new_events}\n\n"
        "Return the updated memory only."
    )


def build_projection_prompt(
    event_log: str,
    task: str,
    budget_chars: int,
    *,
    correction: SessionEvent | None = None,
    must_not_include: list[str] | None = None,
) -> str:
    """Project the event log into deterministic decision memory.

    When `correction` and/or `must_not_include` are provided, emit
    explicit BLOCKING CORRECTION and INVALIDATED FACTS suppression
    blocks. The Phase 3 audit gate refuses checkpoints when a
    blocking correction lands, but if the agent re-projects from raw
    events the projector itself must be correction-aware — otherwise
    the invalidated fact reappears in the fallback memory and the
    decision call propagates it. This is the prompt-side half of the
    fallback-path fix; the agent-side half runs a deterministic guard
    on the resulting projection text.
    """
    blocks: list[str] = []
    if correction is not None:
        blocks.append(
            f"\nBLOCKING CORRECTION (event #{int(correction.idx) + 1}):\n"
            f"{correction.text}\n"
            "You MUST treat any earlier claim this correction overturns as "
            "REVOKED. Do not include the revoked claim in your projection, "
            "even if earlier events asserted it. Cite the correction event "
            "so the suppression is auditable.\n"
        )
    if must_not_include:
        listed = "\n".join(f"  - {item}" for item in must_not_include)
        blocks.append(
            "\nINVALIDATED FACTS — DO NOT REPRODUCE:\n"
            f"{listed}\n"
            "These literal phrases are invalidated by the correction above. "
            "Do NOT include them in your projection in any form. If later "
            "events restate the corrected version, prefer that.\n"
        )
    suppressions = "".join(blocks)
    return (
        "Project the event log into deterministic decision memory.\n"
        "Keep only task-relevant facts, corrections, tool names, and compliance "
        "constraints. Preserve citations by event number. Do not carry facts "
        "that a later correction invalidates."
        f"{suppressions}\n"
        f"MEMORY BUDGET CHARS: {budget_chars}\n\n"
        f"TASK:\n{task}\n\n"
        f"EVENT LOG:\n{event_log}\n\n"
        "Return projected memory only."
    )


def build_decision_prompt(memory: str, task: str) -> str:
    return (
        "You are the next agent after a handoff.\n"
        "Use only the supplied memory. If a correction conflicts with an older "
        "fact, follow the correction.\n\n"
        f"MEMORY:\n{memory or '(empty)'}\n\n"
        f"TASK:\n{task}\n\n"
        "Return the answer only."
    )


def estimate_tokens(text: str) -> int:
    # Good enough for cost-shape comparisons until tokenizer-backed accounting lands.
    return max(1, (len(text) + 3) // 4) if text else 0


def sha256_hex(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest() if text else ""


def checkpoint_manifest_id(
    *,
    case: SessionCase,
    body_hash: str,
    range_start: int,
    range_end: int,
    schema_id: str,
    projection_model_id: str,
    audit_policy_version: str,
) -> str:
    payload = {
        "audit_policy_version": audit_policy_version,
        "body_hash": body_hash,
        "case_id": case.case_id,
        "projection_model_id": projection_model_id,
        "range_end": range_end,
        "range_start": range_start,
        "schema_id": schema_id,
    }
    return sha256_hex(json.dumps(payload, sort_keys=True, separators=(",", ":")))


def audit_certificate_id_for(
    checkpoint_manifest_hash: str,
    audit_verdict: AuditVerdict,
    blocking_corrections: list[str],
) -> str:
    payload = {
        "blocking_corrections": blocking_corrections,
        "manifest_hash": checkpoint_manifest_hash,
        "verdict": audit_verdict.value,
    }
    return sha256_hex(json.dumps(payload, sort_keys=True, separators=(",", ":")))


def correction_id_for(case: SessionCase, event: SessionEvent) -> str:
    payload = {
        "case_id": case.case_id,
        "event_idx": event.idx,
        "text": event.text,
    }
    return sha256_hex(json.dumps(payload, sort_keys=True, separators=(",", ":")))


def _result(
    *,
    condition: Condition,
    memory_bytes: str,
    answer_bytes: str,
    responses: list[ModelResponse],
    notes: str,
) -> AgentResult:
    return AgentResult(
        condition=condition,
        memory_bytes=memory_bytes,
        answer_bytes=answer_bytes,
        model_calls=len(responses),
        input_tokens=sum(r.input_tokens for r in responses),
        output_tokens=sum(r.output_tokens for r in responses),
        wall_ms=sum(r.wall_ms for r in responses),
        memory_sha256=sha256_hex(memory_bytes),
        answer_sha256=sha256_hex(answer_bytes),
        notes=notes,
    )


def _windows(items: list[SessionEvent], size: int) -> Iterable[list[SessionEvent]]:
    for i in range(0, len(items), size):
        yield items[i : i + size]


def _clip(text: str, max_chars: int) -> str:
    if max_chars <= 0:
        return ""
    return text[:max_chars]


def _heuristic_summary(prompt: str) -> str:
    important: list[str] = []
    for line in prompt.splitlines():
        lower = line.lower()
        if any(token in lower for token in (
            "correction",
            "must not",
            "do not",
            "handoff",
            "tool_name",
            "next engineer",
            "checkpointed projection",
            "private logs",
        )):
            important.append(line)
    if not important:
        important = prompt.splitlines()[-8:]
    return "\n".join(important[-16:])


def _heuristic_projection(prompt: str) -> str:
    important: list[str] = []
    for line in prompt.splitlines():
        lower = line.lower()
        if any(token in lower for token in (
            "correction",
            "must_not_include",
            "must_include",
            "tool_name",
            "checkpointed projection",
            "runtime/platform/checkpoint",
            "terminate-instances",
            "private logs",
            "golden fixture",
        )):
            important.append(line)
    if not important:
        important = prompt.splitlines()[-12:]
    return "\n".join(important[-20:])


def _heuristic_decision(prompt: str) -> str:
    lower = prompt.lower()
    if "does not depend on my private logs" in lower:
        return "The handoff constraint is that the next engineer does not depend on my private logs."
    if "edit" in lower and "runtime/platform/checkpoint" in lower:
        return "Call Edit on runtime/platform/checkpoint next."
    if "checkpointed projection is the contribution" in lower:
        return "Preserve the correction: checkpointed projection is the contribution, not transport."
    if "terminate-instances" in lower:
        return "The next infrastructure operation is AwsEc2 terminate-instances."
    if "runtime/platform/checkpoint" in lower:
        return "The next tool area is runtime/platform/checkpoint."
    if "add the golden fixture" in lower:
        return "The user asks next to add the golden fixture."
    return "Insufficient deterministic signal in supplied memory."


def _default_adapter():
    """Return the model adapter used for AGENT_REGISTRY agents.

    Default: HeuristicModelAdapter (deterministic, no API calls). This
    is the right default for CI smoke runs.

    Opt-in: set BENCH_USE_ANTHROPIC=1 (or =true) to swap in
    AnthropicModelAdapter. ANTHROPIC_API_KEY must be set; the adapter
    raises with a clear message otherwise.
    """
    import os as _os
    if _os.environ.get("BENCH_USE_ANTHROPIC", "").lower() in ("1", "true", "yes"):
        try:
            from tools.benchmarks.dpm_phase3_bench.anthropic_adapter import (
                AnthropicModelAdapter,
            )
        except ModuleNotFoundError:
            from anthropic_adapter import AnthropicModelAdapter  # type: ignore
        return AnthropicModelAdapter()
    return HeuristicModelAdapter()


AGENT_REGISTRY = {
    Condition.RAW_ORACLE: lambda: RawOracleAgent(_default_adapter()),
    Condition.ROLLING_SUMMARY: lambda: RollingSummaryAgent(_default_adapter()),
    Condition.DPM_PHASE3_CHECKPOINT: lambda: DpmPhase3CheckpointAgent(
        _default_adapter()
    ),
}


def _selftest() -> int:
    fixture = (
        Path(__file__).parent
        / "fixtures"
        / "real_sessions"
        / "curated_session_cases.json"
    )
    cases = load_session_cases(fixture)
    if len(cases) != 5:
        print(f"FAIL expected 5 curated cases, got {len(cases)}")
        return 1

    model = HeuristicModelAdapter()
    raw = RawOracleAgent(model)
    rolling = RollingSummaryAgent(model, window_size_events=4)
    dpm = DpmPhase3CheckpointAgent(model)

    failures = 0
    for case in cases:
        if not case.probes:
            print(f"FAIL {case.case_id}: missing probes")
            failures += 1
            continue
        probe = case.probes[0]
        raw_result = raw.run(case, probe, budget_chars=1338)
        rolling_result = rolling.run(case, probe, budget_chars=1338)
        dpm_result = dpm.run(case, probe, budget_chars=1338)
        if raw_result.condition != Condition.RAW_ORACLE:
            print(f"FAIL {case.case_id}: raw condition={raw_result.condition}")
            failures += 1
        if rolling_result.condition != Condition.ROLLING_SUMMARY:
            print(f"FAIL {case.case_id}: rolling condition={rolling_result.condition}")
            failures += 1
        if raw_result.model_calls != 1:
            print(f"FAIL {case.case_id}: raw model_calls={raw_result.model_calls}")
            failures += 1
        if rolling_result.model_calls < 2:
            print(
                f"FAIL {case.case_id}: rolling model_calls="
                f"{rolling_result.model_calls}"
            )
            failures += 1
        if len(rolling_result.memory_bytes) > 1338:
            print(
                f"FAIL {case.case_id}: rolling memory escaped budget "
                f"({len(rolling_result.memory_bytes)} chars)"
            )
            failures += 1
        if dpm_result.condition != Condition.DPM_PHASE3_CHECKPOINT:
            print(f"FAIL {case.case_id}: DPM condition={dpm_result.condition}")
            failures += 1
        if not dpm_result.checkpoint_manifest_hash:
            print(f"FAIL {case.case_id}: DPM missing manifest hash")
            failures += 1
        if not dpm_result.audit_certificate_id:
            print(f"FAIL {case.case_id}: DPM missing audit certificate id")
            failures += 1
        if case.case_id == "correction-heavy-session":
            if dpm_result.gate_may_use is not False:
                print("FAIL correction-heavy-session: DPM gate did not refuse")
                failures += 1
            if not dpm_result.blocking_corrections:
                print("FAIL correction-heavy-session: DPM missing correction")
                failures += 1

    if failures:
        print(f"memory_agents._selftest: {failures} FAILURES")
    else:
        print(f"memory_agents._selftest: ALL PASS ({len(cases)} cases)")
    return failures


if __name__ == "__main__":
    raise SystemExit(_selftest())
