"""Ingester: agent session JSONL -> SessionCase corpus.

Reads a Claude Code or Codex session log, normalizes events, redacts
credentials in place, picks probe-points, derives ground-truth answers
from later turns, and emits one or more SessionCase records as JSON.

Probe kinds:
  - next_user_intent: at turn T, what does the user ask at T+1?
    Ground truth: substring of the next user message text.
  - next_tool_call: at turn T, which tool does the agent invoke next?
    Ground truth: tool name + first arg (e.g. "Bash:git status").
  - decision_recall: at turn T, given a question about an earlier
    decision, does the agent's answer match what actually got committed
    or done in the next K turns? Ground truth: derived from key
    phrases the user OR the agent committed to in the trajectory.
  - correction_detection: at turn T, was a prior decision overruled?
    Ground truth: substring "actually" / "pull out" / "drift" / etc.
    flagged as correction events in nearby turns.

Usage:
  python ingest_session.py <path/to/session.jsonl> > out.json
  python ingest_session.py <path/to/session.jsonl> --case-id NAME
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from redactor import redact, looks_redacted


# ---- Normalized event shape ----------------------------------------

@dataclass
class Event:
    """Normalized event the substrate / probes operate on. We collapse
    Claude's tree-shaped tool-use messages into a flat sequence so the
    downstream property tests don't have to know the source format."""
    idx: int               # 0-based position in the session
    kind: str              # "user" | "assistant_text" | "assistant_thinking"
                           # | "tool_call" | "tool_result" | "system"
    role: str              # "user" | "assistant" | "system" | "tool"
    text: str              # rendered text of the event (already redacted)
    timestamp: str         # ISO8601
    tool_name: str = ""    # set when kind == tool_call/tool_result
    tool_args: str = ""    # short repr of arguments
    raw_uuid: str = ""     # session uuid for tracing


@dataclass
class Probe:
    kind: str              # "next_user_intent" | "next_tool_call" |
                           # "decision_recall" | "correction_detection"
    question: str          # what the agent under test will be asked
    expected_match: dict   # ground truth, shape varies by kind
    rationale: str         # why this probe was generated (audit trail)


@dataclass
class SessionCase:
    case_id: str
    domain: str            # "claude" | "codex"
    source_path: str
    source_sha256: str     # hash of the source file BEFORE redaction
    n_events: int          # total normalized events
    probe_T: int           # the turn we stopped at
    events: list[Event] = field(default_factory=list)
    probes: list[Probe] = field(default_factory=list)


# ---- Format-specific extractors ------------------------------------

def _flatten_assistant_content(content: Any) -> list[tuple[str, str, str, str]]:
    """Claude assistant messages have content = list of blocks
    (text / thinking / tool_use). Returns [(kind, text, tool_name, tool_args), ...]
    in source order. Empty thinking blocks are skipped."""
    out = []
    if isinstance(content, str):
        if content.strip():
            out.append(("assistant_text", content, "", ""))
        return out
    if not isinstance(content, list):
        return out
    for block in content:
        if not isinstance(block, dict):
            continue
        t = block.get("type")
        if t == "text":
            text = block.get("text", "")
            if text:
                out.append(("assistant_text", text, "", ""))
        elif t == "thinking":
            text = block.get("thinking", "")
            if text:
                out.append(("assistant_thinking", text, "", ""))
        elif t == "tool_use":
            tool_name = block.get("name", "")
            tool_input = block.get("input", {})
            # First-arg repr: enough for matching, never the whole arg.
            args_repr = ""
            if isinstance(tool_input, dict):
                # Pick the most common arg key for that tool
                for k in ("command", "path", "file_path", "pattern", "url", "query"):
                    if k in tool_input:
                        v = tool_input[k]
                        if isinstance(v, str):
                            args_repr = v[:120]
                            break
                if not args_repr and tool_input:
                    first_k = next(iter(tool_input))
                    v = tool_input[first_k]
                    if isinstance(v, str):
                        args_repr = f"{first_k}={v[:100]}"
            out.append(("tool_call",
                        f"[tool_use {tool_name}({args_repr})]",
                        tool_name, args_repr))
        elif t == "tool_result":
            tool_id = block.get("tool_use_id", "")
            content_val = block.get("content", "")
            if isinstance(content_val, list):
                content_val = " ".join(
                    b.get("text", "") for b in content_val if isinstance(b, dict)
                )
            out.append(("tool_result", str(content_val)[:2000],
                        "", tool_id))
    return out


def _flatten_user_content(content: Any) -> list[str]:
    """User content can be string or list of blocks (with tool_result
    blocks for tool replies). Returns list of text pieces in order."""
    if isinstance(content, str):
        return [content]
    if not isinstance(content, list):
        return []
    out = []
    for block in content:
        if isinstance(block, dict):
            t = block.get("type")
            if t == "text":
                out.append(block.get("text", ""))
            elif t == "tool_result":
                c = block.get("content", "")
                if isinstance(c, list):
                    c = " ".join(b.get("text", "") for b in c if isinstance(b, dict))
                out.append(str(c)[:2000])
    return [x for x in out if x]


def _is_meta_user_msg(text: str) -> bool:
    """Filter out the local-command-caveat / system-injected user
    messages and tool-result-shaped pastes. They aren't real user
    intent and probe-extraction false-positives on them."""
    if not text:
        return True
    if "<local-command-caveat>" in text:
        return True
    if text.startswith("<system-reminder>"):
        return True
    if text.startswith("[Request interrupted by user"):
        return False  # this IS real signal
    # Numbered-line paste (Read tool output etc.): "1\t...\n2\t..."
    # Shows up as a "user" record but is really a tool result.
    if re.match(r"^\s*1\t.+\n\s*2\t", text):
        return True
    # Bash output blocks captured as user messages.
    if text.startswith("Tool ran without output") or \
       text.startswith("<tool_use_error>"):
        return True
    return False


def parse_claude_session(path: Path) -> tuple[list[Event], str]:
    """Parses a Claude .claude/projects/<dir>/<uuid>.jsonl file.
    Returns (events, source_sha256)."""
    h = hashlib.sha256()
    events: list[Event] = []
    idx = 0
    with open(path, "rb") as f:
        raw_bytes = f.read()
    h.update(raw_bytes)
    for line in raw_bytes.splitlines():
        if not line.strip():
            continue
        try:
            d = json.loads(line)
        except Exception:
            continue
        kind = d.get("type")
        ts = d.get("timestamp", "")
        uid = d.get("uuid", "")
        if kind == "user":
            msg = d.get("message", {})
            for piece in _flatten_user_content(msg.get("content", "")):
                if _is_meta_user_msg(piece):
                    continue
                redacted, _ = redact(piece)
                events.append(Event(idx=idx, kind="user", role="user",
                                    text=redacted, timestamp=ts, raw_uuid=uid))
                idx += 1
        elif kind == "assistant":
            msg = d.get("message", {})
            for sub_kind, txt, tn, ta in _flatten_assistant_content(msg.get("content", [])):
                redacted, _ = redact(txt)
                redacted_args, _ = redact(ta)
                events.append(Event(idx=idx, kind=sub_kind, role="assistant",
                                    text=redacted, timestamp=ts,
                                    tool_name=tn, tool_args=redacted_args,
                                    raw_uuid=uid))
                idx += 1
        elif kind == "system":
            txt = d.get("content", "")
            if isinstance(txt, str) and txt.strip():
                redacted, _ = redact(txt)
                events.append(Event(idx=idx, kind="system", role="system",
                                    text=redacted, timestamp=ts, raw_uuid=uid))
                idx += 1
        # silently drop: permission-mode, last-prompt, queue-operation,
        # file-history-snapshot, attachment — not signal for our probes.
    return events, h.hexdigest()


def parse_codex_session(path: Path) -> tuple[list[Event], str]:
    """Codex rollout JSONL: each line is {type, timestamp, payload}.
    Payload is a Responses API event."""
    h = hashlib.sha256()
    events: list[Event] = []
    idx = 0
    with open(path, "rb") as f:
        raw_bytes = f.read()
    h.update(raw_bytes)
    for line in raw_bytes.splitlines():
        if not line.strip():
            continue
        try:
            d = json.loads(line)
        except Exception:
            continue
        ts = d.get("timestamp", "")
        payload = d.get("payload", {})
        ptype = (payload.get("type", "") if isinstance(payload, dict) else "")
        # Best-effort mapping. The Codex format is roughly OpenAI
        # Responses API events; we just want to surface the user/agent
        # turns and tool calls.
        if ptype == "message":
            role = payload.get("role", "user")
            content = payload.get("content", "")
            if isinstance(content, list):
                content = " ".join(
                    c.get("text", "") if isinstance(c, dict) else str(c)
                    for c in content
                )
            if not isinstance(content, str) or not content.strip():
                continue
            redacted, _ = redact(content)
            events.append(Event(idx=idx, kind=("user" if role == "user" else "assistant_text"),
                                role=role, text=redacted, timestamp=ts))
            idx += 1
        elif ptype in ("function_call", "tool_call"):
            name = payload.get("name", "")
            args = payload.get("arguments", "")
            if isinstance(args, dict):
                args = json.dumps(args)
            redacted_args, _ = redact(args[:200] if isinstance(args, str) else "")
            events.append(Event(idx=idx, kind="tool_call", role="assistant",
                                text=f"[tool_call {name}({redacted_args})]",
                                timestamp=ts, tool_name=name, tool_args=redacted_args))
            idx += 1
        elif ptype in ("function_call_output", "tool_result"):
            output = payload.get("output", "")
            if isinstance(output, list):
                output = " ".join(
                    o.get("text", "") if isinstance(o, dict) else str(o)
                    for o in output
                )
            redacted, _ = redact(str(output)[:2000])
            events.append(Event(idx=idx, kind="tool_result", role="tool",
                                text=redacted, timestamp=ts))
            idx += 1
    return events, h.hexdigest()


# ---- Probe-point selection -----------------------------------------

# Keywords that signal a correction is being made. Tuned from this
# repo's actual session content.
CORRECTION_KEYWORDS = [
    "you drifted", "180'd", "you went off", "wait wait", "stop doing",
    "no actually", "actually use", "pull out", "wrong", "redo",
    "let's not", "don't do that", "i misspoke", "scratch that",
    "correction", "i changed my mind", "instead let's", "back up",
    "course-correct", "course correct", "got off track", "off the path",
]


def _has_correction_signal(text: str) -> bool:
    if not text:
        return False
    low = text.lower()
    # Exclude harness-generated summary blocks. They contain the word
    # "correction" because they describe the conversation's content,
    # not because the user is making one.
    if "this session is being continued from a previous conversation" in low:
        return False
    if low.startswith("conversation compacted"):
        return False
    if "summary:" in low and "primary request and intent:" in low:
        return False
    # Real corrections are short user messages, not pasted blocks.
    if len(text) > 1500:
        return False
    return any(kw in low for kw in CORRECTION_KEYWORDS)


def _pick_probe_points(events: list[Event]) -> list[int]:
    """Returns a small set of indices that are good probe-points.
    Heuristics:
      - One early (~10% in)
      - One mid (~50%)
      - One late (~80%)
      - Plus any user turn that contains a correction keyword.
    Each probe-point T is chosen so events[T+1..T+5] exist (we need
    at least one ground-truth turn after).
    """
    n = len(events)
    if n < 12:
        return []
    fixed = [n // 10, n // 2, (n * 4) // 5]
    found = []
    for ev in events:
        if ev.kind == "user" and _has_correction_signal(ev.text):
            if ev.idx >= 5 and ev.idx < n - 5:
                found.append(ev.idx)
    seen = set()
    out = []
    for t in fixed + found[:5]:
        if t in seen:
            continue
        seen.add(t)
        if t < 5 or t >= n - 5:
            continue
        out.append(t)
    return sorted(out)


def _probe_next_user_intent(events: list[Event], T: int) -> Probe | None:
    """At turn T, what does the user say next?"""
    for j in range(T + 1, min(T + 8, len(events))):
        if events[j].kind == "user" and len(events[j].text) > 16:
            text = events[j].text
            # Take the first sentence-ish chunk as the expected match.
            chunk = re.split(r"[.?!\n]", text, maxsplit=1)[0].strip()[:200]
            return Probe(
                kind="next_user_intent",
                question="Based on the projection, what is the user about to ask?",
                expected_match={"substring": chunk[:80]},
                rationale=f"next_user_at_turn_{j}",
            )
    return None


def _probe_next_tool_call(events: list[Event], T: int) -> Probe | None:
    """At turn T, which tool does the agent call next?"""
    for j in range(T + 1, min(T + 12, len(events))):
        if events[j].kind == "tool_call":
            return Probe(
                kind="next_tool_call",
                question="What tool will the agent invoke next, and what is its primary argument?",
                expected_match={
                    "tool_name": events[j].tool_name,
                    "arg_substring": events[j].tool_args[:80],
                },
                rationale=f"next_tool_at_turn_{j}",
            )
    return None


def _probe_correction_detection(events: list[Event], T: int) -> Probe | None:
    """At turn T (which we know contains a correction signal), assert
    the agent recognizes the correction relative to a prior decision."""
    if not _has_correction_signal(events[T].text):
        return None
    return Probe(
        kind="correction_detection",
        question="Did the user just overrule a prior decision? If so, what was overruled?",
        expected_match={
            "correction_substring": events[T].text[:140],
            "must_acknowledge": "correction",
        },
        rationale=f"correction_at_turn_{T}",
    )


def make_session_case(events: list[Event], path: Path,
                      source_sha: str, case_id: str,
                      domain: str) -> list[SessionCase]:
    """One session can yield multiple SessionCases — one per probe
    point. We share the events prefix; only probes differ."""
    cases = []
    Ts = _pick_probe_points(events)
    for T in Ts:
        probes: list[Probe] = []
        for fn in (_probe_next_user_intent,
                   _probe_next_tool_call,
                   _probe_correction_detection):
            p = fn(events, T)
            if p is not None:
                probes.append(p)
        if not probes:
            continue
        cases.append(SessionCase(
            case_id=f"{case_id}@T={T}",
            domain=domain,
            source_path=str(path),
            source_sha256=source_sha,
            n_events=len(events),
            probe_T=T,
            events=events[:T],
            probes=probes,
        ))
    return cases


# ---- CLI ------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("session_path", type=Path)
    ap.add_argument("--case-id", default=None)
    ap.add_argument("--domain", choices=("claude", "codex", "auto"), default="auto")
    ap.add_argument("--summary", action="store_true",
                    help="Print just stats, not the full case JSON")
    args = ap.parse_args()

    p = args.session_path
    if not p.exists():
        print(f"no such file: {p}", file=sys.stderr); return 2

    if args.domain == "auto":
        # Codex sessions live under .codex/sessions/<year>/...
        domain = "codex" if ".codex" in str(p).replace("\\", "/") else "claude"
    else:
        domain = args.domain

    if domain == "claude":
        events, sha = parse_claude_session(p)
    else:
        events, sha = parse_codex_session(p)

    if not events:
        print(f"no parseable events in {p}", file=sys.stderr); return 1

    case_id = args.case_id or f"{domain}-{p.stem[:36]}"
    cases = make_session_case(events, p, sha, case_id, domain)

    if args.summary:
        print(f"source:        {p}")
        print(f"source_sha256: {sha}")
        print(f"events:        {len(events)}")
        kinds = {}
        for e in events:
            kinds[e.kind] = kinds.get(e.kind, 0) + 1
        for k, v in sorted(kinds.items(), key=lambda x: -x[1]):
            print(f"  {v:>5}  {k}")
        print(f"probe-points:  {len(cases)}")
        for c in cases:
            print(f"  T={c.probe_T:>5}  probes={[p.kind for p in c.probes]}")
        # Audit redactor coverage
        any_redacted = sum(1 for e in events if looks_redacted(e.text))
        print(f"redacted_events: {any_redacted} / {len(events)}")
        return 0

    print(json.dumps([asdict(c) for c in cases], indent=2, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
