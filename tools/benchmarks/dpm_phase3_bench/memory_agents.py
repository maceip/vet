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
    from bench_schema import Condition, ScoreStatus  # type: ignore
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

    def __init__(self, model: ModelAdapter, *, max_context_chars: int = 200_000):
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


def load_session_cases(path: str | Path) -> list[SessionCase]:
    return _load_typed_session_cases(path)


def events_up_to_probe(case: SessionCase) -> list[SessionEvent]:
    events = list(case.events)
    probe_t = int(case.probe_T or len(events))
    return [event for event in events if int(event.idx) <= probe_t]


def render_event_log(case: SessionCase) -> str:
    return render_events(events_up_to_probe(case))


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


def _heuristic_decision(prompt: str) -> str:
    lower = prompt.lower()
    if "checkpointed projection is the contribution" in lower:
        return "Preserve the correction: checkpointed projection is the contribution, not transport."
    if "terminate-instances" in lower:
        return "The next infrastructure operation is AwsEc2 terminate-instances."
    if "runtime/platform/checkpoint" in lower:
        return "The next tool area is runtime/platform/checkpoint."
    if "does not depend on my private logs" in lower:
        return "The handoff constraint is that the next engineer must not depend on private logs."
    if "add the golden fixture" in lower:
        return "The user asks next to add the golden fixture."
    return "Insufficient deterministic signal in supplied memory."


AGENT_REGISTRY = {
    Condition.RAW_ORACLE: lambda: RawOracleAgent(HeuristicModelAdapter()),
    Condition.ROLLING_SUMMARY: lambda: RollingSummaryAgent(HeuristicModelAdapter()),
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

    failures = 0
    for case in cases:
        if not case.probes:
            print(f"FAIL {case.case_id}: missing probes")
            failures += 1
            continue
        probe = case.probes[0]
        raw_result = raw.run(case, probe, budget_chars=1338)
        rolling_result = rolling.run(case, probe, budget_chars=1338)
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

    if failures:
        print(f"memory_agents._selftest: {failures} FAILURES")
    else:
        print(f"memory_agents._selftest: ALL PASS ({len(cases)} cases)")
    return failures


if __name__ == "__main__":
    raise SystemExit(_selftest())
