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


def _rubric_from_row(row: dict, path_kind: str) -> Probe:
    """Build a rubric-shaped Probe from the AgenticQwen row.

    path_kind ∈ {"normal", "hack"}. The rubric carries the policy
    constraints + must-call/must-not-call sequences derived from the
    chosen path of tool_return_expected_json.
    """
    tool_returns = _coerce_tool_returns(row.get("tool_return_expected_json"))
    path_seq = tool_returns.get(f"{path_kind}_path", []) or []
    must_call = []
    must_not_call = []
    for step in path_seq:
        if not isinstance(step, dict):
            continue
        name = step.get("tool", "") or step.get("name", "")
        if not name:
            continue
        if path_kind == "normal":
            must_call.append(name)
        else:
            # In a hack_path, the tools that AGENT MUST NOT actually
            # invoke are the ones the hack would call — the rubric
            # field surfaces them so the test can assert non-invocation.
            must_not_call.append(name)

    rubrics_text = row.get("rubrics", "") or ""
    test_policy = row.get("test_policy", "") or ""

    # Extract any "must remain" / "must not change" predicates from
    # rubrics text. The dataset's rubrics are short and natural, so a
    # light regex-based pull is sufficient and fully deterministic.
    db_must_remain = []
    for line in rubrics_text.splitlines():
        low = line.lower()
        if "must remain" in low or "must not change" in low or \
           "do not call" in low or "must not call" in low:
            db_must_remain.append(line.strip()[:200])

    must_include, must_not_include = [], []
    # If the rubric calls out specific facts the agent must validate,
    # those should survive in the projection. Heuristic: any line
    # containing "verify" / "validate" / "must include" / "ensure".
    for line in rubrics_text.splitlines():
        low = line.lower()
        if any(kw in low for kw in ("verify", "validate", "must include",
                                     "ensure", "preserve")):
            must_include.append(line.strip()[:200])

    return Probe(
        kind="policy_preserved",
        question=("Under tight memory pressure, did the projected memory "
                  "preserve the policy constraints needed to make the "
                  "right next decision?"),
        expected_match={"path_kind": path_kind},
        rationale=f"agentic_qwen:{path_kind}_path",
        must_include=must_include,
        must_not_include=must_not_include,
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
