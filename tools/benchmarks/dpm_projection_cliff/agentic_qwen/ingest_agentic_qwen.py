"""AgenticQwen-Data → SessionCase adapter.

Adapts rows from `alibaba-pai/AgenticQwen-Data` (Hugging Face,
`arXiv:2604.21590`) into the same SessionCase shape the substrate's
scenario tests already consume. Each AgenticQwen row carries explicit
machine-checkable rubrics — must-call-tools, must-not-call-tools,
database-state-must-remain — that fit our newly-extended ProbeRubric
field exactly.

Each AgenticQwen row produces TWO SessionCases linked by paired_case_id:
  - <id>:normal  driven by tool_return_expected_json.normal_path
  - <id>:hack    driven by tool_return_expected_json.hack_path

The pair lets a scenario test run a *differential* assertion: the
projected memory must preserve the policy-relevant facts under BOTH
paths, but only the hack path additionally surfaces the policy-violation
rationale. Catches drift in DPM's task-conditioned compression that a
single-trajectory test would miss.

> SECONDARY VALIDATION ONLY. AgenticQwen examples fit in one context
> window (~7-15K tokens each) so they cannot exercise the cross-context /
> hierarchical / replay-from-raw / Merkle-DAG claims that are Phase 2's
> primary proof. They DO let us test whether DPM's projection retains
> policy-critical constraints under tight memory budgets — which is the
> task-conditioned compression claim. See agentic_qwen/README.md.

Schema reference (as of 2026-04 dataset card):
  id, system, user, task_background, rubrics, test_policy,
  user_escape_strategy, messages_json, tool_return_expected_json

The adapter accepts:
  - A path to a Hugging Face parquet file (after `huggingface-cli
    download alibaba-pai/AgenticQwen-Data`)
  - A path to a local JSONL of one-row-per-line (test fixtures)
  - A single JSON object on stdin (for one-off CLI use)

Usage:
  # Single-row smoke (synthetic fixture):
  python ingest_agentic_qwen.py agentic_qwen/golden/synthetic_seed_row.json

  # Full dataset:
  python ingest_agentic_qwen.py path/to/train-00000-of-00001.parquet \\
      --limit 100 --out cases.json
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import asdict
from pathlib import Path
from typing import Any, Iterable

# Reuse SessionCase / SessionEvent / Probe from the existing scenario
# pipeline so all corpora share one downstream format.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scenario"))
from ingest_session import Event, Probe, SessionCase  # noqa: E402
from redactor import redact  # noqa: E402


# ---- Row decoding --------------------------------------------------

def _coerce_messages(raw: Any) -> list[dict]:
    """messages_json may be a JSON string or a list. Returns list."""
    if isinstance(raw, list):
        return raw
    if isinstance(raw, str):
        try:
            parsed = json.loads(raw)
            if isinstance(parsed, list):
                return parsed
        except Exception:
            return []
    return []


def _coerce_tool_returns(raw: Any) -> dict:
    """tool_return_expected_json may be a JSON string or dict.
    Expected shape: {"normal_path": [...], "hack_path": [...]}"""
    if isinstance(raw, dict):
        return raw
    if isinstance(raw, str):
        try:
            parsed = json.loads(raw)
            if isinstance(parsed, dict):
                return parsed
        except Exception:
            return {}
    return {}


def _events_from_messages(messages: list[dict]) -> list[Event]:
    """Convert AgenticQwen messages_json shape into normalized events.
    The dataset uses Anthropic/OpenAI-style messages with role + content
    + (optional) tool_calls + tool_call_id."""
    events: list[Event] = []
    idx = 0
    for m in messages:
        if not isinstance(m, dict):
            continue
        role = m.get("role", "")
        content = m.get("content", "")
        if isinstance(content, list):
            content = " ".join(
                c.get("text", "") if isinstance(c, dict) else str(c)
                for c in content
            )
        if not isinstance(content, str):
            content = json.dumps(content)
        text_red, _ = redact(content)
        if role == "user" and text_red.strip():
            events.append(Event(idx=idx, kind="user", role="user",
                                text=text_red, timestamp=""))
            idx += 1
        elif role in ("assistant", "model"):
            if text_red.strip():
                events.append(Event(idx=idx, kind="assistant_text",
                                    role="assistant", text=text_red,
                                    timestamp=""))
                idx += 1
            for tc in m.get("tool_calls", []) or []:
                if not isinstance(tc, dict):
                    continue
                fn = tc.get("function") or tc
                name = fn.get("name", "")
                args = fn.get("arguments", "")
                if isinstance(args, dict):
                    args = json.dumps(args)
                args_red, _ = redact(str(args)[:500])
                events.append(Event(idx=idx, kind="tool_call",
                                    role="assistant",
                                    text=f"[tool_use {name}({args_red[:120]})]",
                                    timestamp="",
                                    tool_name=name, tool_args=args_red))
                idx += 1
        elif role == "tool":
            tool_red, _ = redact(text_red[:2000])
            events.append(Event(idx=idx, kind="tool_result", role="tool",
                                text=tool_red, timestamp="",
                                tool_name=m.get("name", "")))
            idx += 1
        elif role == "system" and text_red.strip():
            events.append(Event(idx=idx, kind="system", role="system",
                                text=text_red, timestamp=""))
            idx += 1
    return events


_NUMBERED_CLAUSE_RE = re.compile(r"\s*\d+\)\s*")
_TOOL_TOKEN_RE = re.compile(r"`([a-z][a-z0-9_]+)`|\b([a-z][a-z0-9_]{4,40}_(?:record|address|registration|account|billing|status|date|category|residency|service|number|info|contact|document))\b")


def _split_rubric_clauses(rubrics_text: str) -> list[str]:
    """The real AgenticQwen-Data rubrics are single-line strings with
    numbered clauses ("1) ... 2) ... 3) ..."). My initial line-split
    heuristic missed almost everything. This split extracts each
    numbered clause as a separate string."""
    if not rubrics_text:
        return []
    parts = _NUMBERED_CLAUSE_RE.split(rubrics_text)
    out = [p.strip() for p in parts if p and p.strip()]
    if len(out) >= 2:
        return out
    # Fall back to lines if no numbered clauses present.
    return [ln.strip() for ln in rubrics_text.splitlines() if ln.strip()]


def _extract_tool_names(text: str) -> list[str]:
    """Pull plausible tool names out of a clause. Tools in this dataset
    follow a snake_case `verb_noun_object` pattern (`query_resident_record`,
    `register_new_utility_account`, etc.)."""
    out = []
    seen = set()
    for m in _TOOL_TOKEN_RE.finditer(text):
        token = m.group(1) or m.group(2)
        if token and token not in seen:
            seen.add(token)
            out.append(token)
    return out


def _rubric_from_row(row: dict, path_kind: str) -> Probe:
    """Build a rubric-shaped Probe from the AgenticQwen row.

    path_kind ∈ {"normal", "hack"}. The rubric carries the policy
    constraints + must-call/must-not-call sequences derived from BOTH
    the chosen path of tool_return_expected_json AND the rubrics text
    (the real dataset puts the must-call/must-not-call assertions
    inline in the rubrics prose, not just as path step lists).
    """
    tool_returns = _coerce_tool_returns(row.get("tool_return_expected_json"))

    def _tools_for_path(p: str) -> list[str]:
        out, dedup = [], set()
        for step in tool_returns.get(f"{p}_path", []) or []:
            if not isinstance(step, dict):
                continue
            name = (step.get("tool_name") or step.get("tool")
                    or step.get("name") or "")
            if name and name not in dedup:
                dedup.add(name)
                out.append(name)
        return out

    normal_tools = _tools_for_path("normal")
    hack_tools = _tools_for_path("hack")
    normal_set, hack_set = set(normal_tools), set(hack_tools)

    must_call = []
    must_not_call = []
    if path_kind == "normal":
        # Tools the policy-compliant trajectory invokes.
        must_call = list(normal_tools)
    else:
        # Tools the hack invokes that the normal does NOT — i.e. the
        # policy-violating subset. Tools shared by both paths (e.g.
        # an initial query) are legitimate; including them as
        # must_not_call would contradict normal_path's must_call.
        must_not_call = [t for t in hack_tools if t not in normal_set]
    seen = set(normal_set | hack_set)

    rubrics_text = row.get("rubrics", "") or ""
    test_policy = row.get("test_policy", "") or ""
    clauses = _split_rubric_clauses(rubrics_text)

    db_must_remain = []
    must_include = []
    rubric_must_call = []
    rubric_must_not_call = []
    for clause in clauses:
        low = clause.lower()
        is_must_not = ("must not call" in low or "must not invoke" in low
                       or "do not call" in low or "must not " in low)
        is_must_call = (("must call" in low or "must invoke" in low
                        or "must query" in low or "must verify" in low
                        or "must check" in low) and not is_must_not)
        is_state_invariant = ("must remain" in low or "must not change" in low
                              or "must stay" in low or "no new " in low)
        is_factual = any(kw in low for kw in (
            "must explain", "must include", "must contain",
            "must reference", "response must"))

        if is_must_not:
            for t in _extract_tool_names(clause):
                if t not in seen:
                    rubric_must_not_call.append(t)
                    seen.add(t)
        if is_must_call:
            for t in _extract_tool_names(clause):
                if t not in seen:
                    rubric_must_call.append(t)
                    seen.add(t)
        if is_state_invariant:
            db_must_remain.append(clause[:240])
        if is_factual:
            must_include.append(clause[:240])

    # Merge the path-derived list with the rubric-text-derived list.
    # path-derived takes precedence (it's structured data), rubric-text
    # adds tools the path didn't enumerate.
    if path_kind == "normal":
        for t in rubric_must_call:
            if t not in must_call:
                must_call.append(t)
    else:
        for t in rubric_must_not_call:
            if t not in must_not_call:
                must_not_call.append(t)

    return Probe(
        kind="policy_preserved",
        question=("Under tight memory pressure, did the projected memory "
                  "preserve the policy constraints needed to make the "
                  "right next decision?"),
        expected_match={"path_kind": path_kind},
        rationale=f"agentic_qwen:{path_kind}_path",
        must_include=must_include,
        must_not_include=[],  # Reserved for adapters that surface
                              # explicit forbidden facts; AgenticQwen's
                              # rubrics structure them as tools/state,
                              # not as facts to omit.
        must_call_tools=must_call,
        must_not_call_tools=must_not_call,
        database_state_must_remain=db_must_remain,
        judge_rubric=test_policy[:600],
    )


def row_to_session_cases(row: dict, source_path: str,
                          source_sha: str) -> list[SessionCase]:
    """Each AgenticQwen row produces TWO SessionCases (normal/hack)
    linked by paired_case_id. Both share the same events; only their
    rubric probes differ."""
    base_id = str(row.get("id", "agentic_qwen-unknown"))[:64]
    messages = _coerce_messages(row.get("messages_json"))
    events = _events_from_messages(messages)
    if not events:
        # Synthesize at least a system+user pair from the row's text
        # fields so empty messages_json doesn't drop the case.
        for src_field, kind in (("system", "system"), ("user", "user")):
            txt = row.get(src_field, "") or ""
            if isinstance(txt, str) and txt.strip():
                red, _ = redact(txt)
                events.append(Event(idx=len(events), kind=kind, role=kind,
                                    text=red, timestamp=""))

    n = len(events)
    probe_T = max(0, n - 1)  # we test "what comes next" at the end of
                              # the conversation given the final state

    cases: list[SessionCase] = []
    for path_kind in ("normal", "hack"):
        case_id = f"{base_id}:{path_kind}"
        twin_id = f"{base_id}:{'hack' if path_kind == 'normal' else 'normal'}"
        c = SessionCase(
            case_id=case_id,
            domain="agentic_qwen",
            source_path=source_path,
            source_sha256=source_sha,
            n_events=n,
            probe_T=probe_T,
            events=events[:probe_T],
            probes=[_rubric_from_row(row, path_kind)],
            paired_case_id=twin_id,
            pair_role=path_kind,
        )
        cases.append(c)
    return cases


# ---- Source file decoding ------------------------------------------

def _iter_rows(path: Path) -> Iterable[dict]:
    suffix = path.suffix.lower()
    if suffix == ".parquet":
        try:
            import pyarrow.parquet as pq  # type: ignore
        except ImportError as e:
            raise SystemExit(
                f"reading parquet requires pyarrow; pip install pyarrow ({e})")
        table = pq.read_table(str(path))
        for row in table.to_pylist():
            yield row
    elif suffix in (".json", ".jsonl"):
        with open(path, "r", encoding="utf-8") as f:
            text = f.read().strip()
        # If it's a single object, yield one. If it's a JSONL, yield each line.
        # If it's a JSON array, yield each element.
        if text.startswith("["):
            arr = json.loads(text)
            for r in arr:
                if isinstance(r, dict):
                    yield r
        elif text.startswith("{") and "\n{" not in text:
            yield json.loads(text)
        else:
            for line in text.splitlines():
                line = line.strip()
                if not line:
                    continue
                try:
                    yield json.loads(line)
                except Exception:
                    continue
    else:
        raise SystemExit(f"unsupported source format: {path.suffix}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("source_path", type=Path,
                    help="Path to AgenticQwen-Data parquet, JSON, or JSONL")
    ap.add_argument("--limit", type=int, default=0,
                    help="Stop after N rows (0 = all). "
                         "Each row produces 2 SessionCases.")
    ap.add_argument("--out", type=Path, default=None,
                    help="Write output JSON here (default stdout)")
    args = ap.parse_args()

    if not args.source_path.exists():
        print(f"no such file: {args.source_path}", file=sys.stderr)
        return 2

    h = hashlib.sha256()
    h.update(args.source_path.read_bytes())
    source_sha = h.hexdigest()

    all_cases: list[SessionCase] = []
    for i, row in enumerate(_iter_rows(args.source_path)):
        if args.limit and i >= args.limit:
            break
        all_cases.extend(row_to_session_cases(row, str(args.source_path),
                                              source_sha))

    payload = json.dumps([asdict(c) for c in all_cases], indent=2,
                         default=str)
    if args.out:
        args.out.write_text(payload, encoding="utf-8")
        print(f"wrote {len(all_cases)} SessionCases to {args.out}",
              file=sys.stderr)
    else:
        print(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
