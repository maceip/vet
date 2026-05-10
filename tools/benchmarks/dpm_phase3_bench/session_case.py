"""Python loader for SessionCase JSON fixtures.

Mirrors the C++ `SessionCase` struct (see
runtime/platform/checkpoint/testing/session_case_loader.h). The runner
loads fixtures through this module; the C++ scenario tests load the
same JSON files through the C++ loader. Single source of truth for the
fixture format.

Schema:

  {
    "case_id": "...",
    "domain": "claude" | "codex" | "agentic_qwen" | ...,
    "source_path": "...",
    "source_sha256": "...",
    "n_events": <int>,
    "probe_T": <int>,
    "events": [
      { "idx": int, "kind": str, "role": str, "text": str,
        "timestamp": str, "tool_name": str, "tool_args": str,
        "raw_uuid": str },
      ...
    ],
    "probes": [
      { "kind": str, "question": str,
        "expected_match": { ...varies by kind... },
        "rubric": { "must_include": [...], "must_not_include": [...],
                    "must_call_tools": [...], "must_not_call_tools": [...],
                    "database_state_must_remain": [...],
                    "judge_rubric": "..." },
        "rationale": str }
    ],
    "paired_case_id": "...",   # optional
    "pair_role": "normal" | "hack" | ""   # optional
  }
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class SessionEvent:
    idx: int = 0
    kind: str = ""
    role: str = ""
    text: str = ""
    timestamp: str = ""
    tool_name: str = ""
    tool_args: str = ""
    raw_uuid: str = ""


@dataclass
class ProbeExpectedMatch:
    """Per-kind expected-match payload. Different probe kinds populate
    different subsets of these fields."""
    substring: str = ""             # next_user_intent / correction_detection
    tool_name: str = ""             # next_tool_call
    arg_substring: str = ""         # next_tool_call
    correction_substring: str = ""  # correction_detection
    must_acknowledge: str = ""      # correction_detection


@dataclass
class ProbeRubric:
    """Rubric-shaped ground truth from AgenticQwen-style probes. May be
    empty for free-form session probes; the scorer treats empty rubric
    as 'no deterministic check available on this axis'."""
    must_include: list[str] = field(default_factory=list)
    must_not_include: list[str] = field(default_factory=list)
    must_call_tools: list[str] = field(default_factory=list)
    must_not_call_tools: list[str] = field(default_factory=list)
    database_state_must_remain: list[str] = field(default_factory=list)
    judge_rubric: str = ""


@dataclass
class SessionProbe:
    kind: str = ""
    question: str = ""
    expected_match: ProbeExpectedMatch = field(default_factory=ProbeExpectedMatch)
    rubric: ProbeRubric = field(default_factory=ProbeRubric)
    rationale: str = ""


@dataclass
class CorrectionDirective:
    """Typed correction declared by the fixture, NOT inferred from text.

    `event_idx` points to the SessionEvent in `case.events` that is the
    user's correction. `invalidated_facts` and `replacement_facts` are
    runtime suppressions/replacements — the agent is allowed to see them
    (they describe what the user said), in contrast to the scoring
    rubric which is evaluator-only.

    Substring-based correction detection (looking for the literal word
    "correction" in event text) was removed after the 2026-05 review
    surfaced false positives on long-real-session: roadmap-doc events
    containing the word "correction" were being treated as user
    corrections and triggering phantom gate refusals.
    """
    event_idx: int = 0
    correction_id: str = ""
    invalidated_facts: list[str] = field(default_factory=list)
    replacement_facts: list[str] = field(default_factory=list)


@dataclass
class SessionCase:
    case_id: str = ""
    domain: str = ""
    source_path: str = ""
    source_sha256: str = ""
    n_events: int = 0
    probe_T: int = 0
    events: list[SessionEvent] = field(default_factory=list)
    probes: list[SessionProbe] = field(default_factory=list)
    correction_directives: list[CorrectionDirective] = field(default_factory=list)
    paired_case_id: str = ""
    pair_role: str = ""

    def event_text_window(self, start: int, end: int) -> list[SessionEvent]:
        return [e for e in self.events if start <= e.idx < end]


def _parse_expected_match(d: dict[str, Any]) -> ProbeExpectedMatch:
    return ProbeExpectedMatch(
        substring=d.get("substring", "") or "",
        tool_name=d.get("tool_name", "") or "",
        arg_substring=d.get("arg_substring", "") or "",
        correction_substring=d.get("correction_substring", "") or "",
        must_acknowledge=d.get("must_acknowledge", "") or "",
    )


def _parse_rubric(d: dict[str, Any]) -> ProbeRubric:
    """Accept either an inline rubric block (probe.rubric) OR
    rubric fields hoisted onto the probe itself (legacy / AgenticQwen
    adapter shape)."""
    return ProbeRubric(
        must_include=list(d.get("must_include") or []),
        must_not_include=list(d.get("must_not_include") or []),
        must_call_tools=list(d.get("must_call_tools") or []),
        must_not_call_tools=list(d.get("must_not_call_tools") or []),
        database_state_must_remain=list(
            d.get("database_state_must_remain") or []),
        judge_rubric=d.get("judge_rubric", "") or "",
    )


def _parse_event(d: dict[str, Any]) -> SessionEvent:
    return SessionEvent(
        idx=int(d.get("idx", 0)),
        kind=d.get("kind", "") or "",
        role=d.get("role", "") or "",
        text=d.get("text", "") or "",
        timestamp=d.get("timestamp", "") or "",
        tool_name=d.get("tool_name", "") or "",
        tool_args=d.get("tool_args", "") or "",
        raw_uuid=d.get("raw_uuid", "") or "",
    )


def _parse_probe(d: dict[str, Any]) -> SessionProbe:
    em = d.get("expected_match") or {}
    # rubric may be inline under "rubric" or hoisted at the probe level.
    rubric_block = d.get("rubric")
    if rubric_block is None:
        rubric_block = d
    return SessionProbe(
        kind=d.get("kind", "") or "",
        question=d.get("question", "") or "",
        expected_match=_parse_expected_match(em),
        rubric=_parse_rubric(rubric_block),
        rationale=d.get("rationale", "") or "",
    )


def _parse_correction_directive(d: dict[str, Any]) -> CorrectionDirective:
    return CorrectionDirective(
        event_idx=int(d.get("event_idx", 0)),
        correction_id=d.get("correction_id", "") or "",
        invalidated_facts=list(d.get("invalidated_facts") or []),
        replacement_facts=list(d.get("replacement_facts") or []),
    )


def parse_session_case(d: dict[str, Any]) -> SessionCase:
    return SessionCase(
        case_id=d.get("case_id", "") or "",
        domain=d.get("domain", "") or "",
        source_path=d.get("source_path", "") or "",
        source_sha256=d.get("source_sha256", "") or "",
        n_events=int(d.get("n_events", 0)),
        probe_T=int(d.get("probe_T", 0)),
        events=[_parse_event(e) for e in (d.get("events") or [])],
        probes=[_parse_probe(p) for p in (d.get("probes") or [])],
        correction_directives=[
            _parse_correction_directive(c)
            for c in (d.get("correction_directives") or [])
        ],
        paired_case_id=d.get("paired_case_id", "") or "",
        pair_role=d.get("pair_role", "") or "",
    )


def load_session_cases(path: Path | str) -> list[SessionCase]:
    """Load one or more SessionCases from a JSON file. Accepts either a
    single object or an array of objects."""
    p = Path(path)
    raw = json.loads(p.read_text(encoding="utf-8"))
    if isinstance(raw, list):
        return [parse_session_case(r) for r in raw]
    return [parse_session_case(raw)]


def iter_fixture_cases(directory: Path | str) -> list[tuple[Path, SessionCase]]:
    """Walk a directory of *.json fixtures, return (path, case) tuples
    in deterministic order (sorted by filename)."""
    root = Path(directory)
    out: list[tuple[Path, SessionCase]] = []
    for path in sorted(root.glob("*.json")):
        if path.name == "MANIFEST.json":
            continue
        for case in load_session_cases(path):
            out.append((path, case))
    return out
