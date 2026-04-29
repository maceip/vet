"""Curate a small golden set of real SessionCases, one per shape category.

Reads session JSONLs from local Claude/Codex log directories, scores
each session along a few axes (length, tool density, correction density,
compaction presence), buckets the population into five categories, picks
one representative per bucket, runs the redacting ingester over it, and
writes the redacted SessionCase JSON to the bench's golden/real_sessions/
directory.

Critical invariants this script enforces:
  - No raw session logs ever touch git. The script reads from local
    paths (Claude: ~/.claude/projects, Codex: ~/.codex/sessions) and
    writes only the post-redaction SessionCase JSON.
  - The golden set is committed verbatim. Substrate property tests
    consume it from a stable checked-in path; they MUST NOT depend on
    the curator script having run, the live log dirs being readable,
    or any private state.
  - Curation is deterministic given the same input population. Output
    file names and case_ids are derived from the source UUIDs, so a
    re-run on the same population will overwrite the same files.

Categories:
  - short              : minimum normalized event count above a floor
  - long               : maximum normalized event count
  - correction_heavy   : highest count of user-correction signals
  - tool_heavy         : highest tool_call density (tool_calls / events)
  - handoff_ish        : session that survived a context compaction
                         (post-compact "This session is being continued
                         from a previous conversation" appears at least
                         once, and the session has substantial events
                         AFTER the compaction)

Usage:
  python curate_real_sessions.py [--out-dir <dir>]
"""
from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import sys
import time
from dataclasses import asdict
from pathlib import Path
from typing import Iterable

# Reuse the ingester so curation goes through the same parser +
# redactor + probe-extraction the substrate tests will see.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from ingest_session import (  # noqa: E402
    parse_claude_session, parse_codex_session, make_session_case,
    SessionCase,
)
from redactor import looks_redacted  # noqa: E402


# ---- Discovery -----------------------------------------------------

def _claude_dirs() -> list[Path]:
    out = []
    for cand in (Path.home() / ".claude" / "projects",
                 Path("C:/Users/mac/.claude/projects")):
        if cand.exists() and cand.is_dir():
            out.append(cand)
            break
    return out


def _codex_dirs() -> list[Path]:
    out = []
    for cand in (Path.home() / ".codex" / "sessions",
                 Path("C:/Users/mac/.codex/sessions")):
        if cand.exists() and cand.is_dir():
            out.append(cand)
            break
    return out


def _enumerate_sessions() -> list[tuple[str, Path]]:
    """Returns [(domain, path)]."""
    out: list[tuple[str, Path]] = []
    for root in _claude_dirs():
        for p in root.rglob("*.jsonl"):
            out.append(("claude", p))
    for root in _codex_dirs():
        for p in root.rglob("*.jsonl"):
            out.append(("codex", p))
    return out


# ---- Scoring -------------------------------------------------------

@dataclasses.dataclass
class SessionScore:
    domain: str
    path: Path
    n_events: int
    n_tool_calls: int
    n_corrections: int
    has_compaction: bool
    events_after_compaction: int
    case: SessionCase | None  # may be None if no probes generated


CORRECTION_KEYWORDS_FAST = (
    "you drifted", "180'd", "wait wait", "stop doing",
    "actually use", "pull out", "let's not", "don't do that",
    "scratch that", "i changed my mind", "instead let's",
    "back up", "course correct", "got off track", "off the path",
)

COMPACTION_MARKER = (
    "This session is being continued from a previous conversation"
)


def _score(domain: str, path: Path) -> SessionScore:
    parser = parse_claude_session if domain == "claude" else parse_codex_session
    try:
        events, _sha = parser(path)
    except Exception:
        return SessionScore(domain, path, 0, 0, 0, False, 0, None)
    n_tool = sum(1 for e in events if e.kind == "tool_call")
    n_corr = 0
    has_comp = False
    events_after_comp = 0
    for i, e in enumerate(events):
        if e.kind != "user":
            continue
        low = (e.text or "").lower()
        if any(kw in low for kw in CORRECTION_KEYWORDS_FAST):
            n_corr += 1
        if COMPACTION_MARKER.lower() in low:
            has_comp = True
            events_after_comp = max(events_after_comp,
                                     len(events) - i - 1)
    case_list = make_session_case(
        events, path,
        hashlib.sha256(path.read_bytes()).hexdigest(),
        f"{domain}-{path.stem[:36]}", domain,
    )
    case = case_list[0] if case_list else None
    return SessionScore(domain=domain, path=path, n_events=len(events),
                         n_tool_calls=n_tool, n_corrections=n_corr,
                         has_compaction=has_comp,
                         events_after_compaction=events_after_comp,
                         case=case)


# ---- Bucket selection ---------------------------------------------

def _pick_buckets(scores: list[SessionScore]) -> dict[str, SessionScore]:
    out: dict[str, SessionScore] = {}
    cands = [s for s in scores if s.case is not None and s.n_events >= 12]
    if not cands:
        return out

    short_cands = [s for s in cands if 12 <= s.n_events <= 80]
    if short_cands:
        out["short"] = min(short_cands, key=lambda s: s.n_events)

    long_cands = sorted(cands, key=lambda s: -s.n_events)
    if long_cands:
        out["long"] = long_cands[0]

    corr_cands = sorted(
        [s for s in cands if s.n_corrections > 0],
        key=lambda s: (-s.n_corrections, s.n_events),
    )
    if corr_cands:
        out["correction_heavy"] = corr_cands[0]

    tool_cands = [s for s in cands if s.n_events >= 50]
    if tool_cands:
        out["tool_heavy"] = max(
            tool_cands,
            key=lambda s: (s.n_tool_calls / max(1, s.n_events))
            + (1 if s.n_tool_calls >= 50 else 0),
        )

    handoff_cands = [s for s in cands
                     if s.has_compaction and s.events_after_compaction >= 30]
    if handoff_cands:
        out["handoff_ish"] = max(handoff_cands,
                                  key=lambda s: s.events_after_compaction)

    # Ensure unique sessions per bucket. If two buckets selected the
    # same session, keep the higher-priority bucket and drop the lower.
    priority = ["correction_heavy", "handoff_ish", "tool_heavy", "long",
                "short"]
    seen_paths: dict[Path, str] = {}
    deduped: dict[str, SessionScore] = {}
    for b in priority:
        s = out.get(b)
        if not s:
            continue
        if s.path in seen_paths:
            continue
        deduped[b] = s
        seen_paths[s.path] = b
    return deduped


# ---- Output --------------------------------------------------------

def _write_case_json(case: SessionCase, out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    # Scrub the raw source_path: it carries the user's home dir,
    # project name, and session UUID. Replace with a short stable
    # identifier derived from the source_sha256 so the case is still
    # traceable but the local filesystem layout doesn't ship.
    sha8 = case.source_sha256[:8] if case.source_sha256 else ""
    case.source_path = f"<curated:{case.domain}:{sha8}>"
    out_path.write_text(json.dumps(asdict(case), indent=2, default=str),
                        encoding="utf-8")


def _write_index(buckets: dict[str, SessionScore], out_dir: Path,
                 generated_at: str) -> None:
    """A small index alongside the JSONs documenting which session went
    where, what its shape was, and why it was picked. Engineer reads
    this; substrate tests do not."""
    rows = []
    for b, s in buckets.items():
        c = s.case
        rows.append({
            "bucket": b,
            "case_id": c.case_id,
            "domain": s.domain,
            "n_events_normalized": s.n_events,
            "n_tool_calls": s.n_tool_calls,
            "n_correction_signals": s.n_corrections,
            "has_compaction": s.has_compaction,
            "events_after_compaction": s.events_after_compaction,
            "n_probes": len(c.probes),
            "probe_kinds": [p.kind for p in c.probes],
            "filename": _bucket_filename(b),
        })
    out = {
        "generated_at": generated_at,
        "curator_script":
            "tools/benchmarks/dpm_projection_cliff/scenario/curate_real_sessions.py",
        "note": ("Raw session paths are intentionally OMITTED from this "
                 "index: only the post-redaction SessionCase JSONs "
                 "alongside this index are committed to git."),
        "buckets": rows,
    }
    (out_dir / "index.json").write_text(
        json.dumps(out, indent=2), encoding="utf-8")


def _bucket_filename(bucket: str) -> str:
    return f"{bucket}.session_case.json"


def _verify_redaction(case: SessionCase) -> int:
    """Returns count of events that reference any matched-secret
    placeholder (i.e. redacted). Mostly diagnostic — lets us confirm
    the redactor is doing its job before we commit."""
    return sum(1 for e in case.events if looks_redacted(e.text))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", type=Path,
                    default=Path(__file__).resolve().parent / "golden"
                            / "real_sessions")
    ap.add_argument("--max-scan", type=int, default=600,
                    help="Cap on sessions to score (default 600)")
    ap.add_argument("--dry-run", action="store_true",
                    help="Score+pick but do not write JSONs")
    args = ap.parse_args()

    sessions = _enumerate_sessions()
    if not sessions:
        print("no sessions found in ~/.claude/projects or "
              "~/.codex/sessions", file=sys.stderr)
        return 2
    if len(sessions) > args.max_scan:
        sessions = sessions[: args.max_scan]
    print(f"scoring {len(sessions)} sessions...", file=sys.stderr)

    scores: list[SessionScore] = []
    for i, (domain, path) in enumerate(sessions):
        if (i + 1) % 50 == 0:
            print(f"  scored {i+1}/{len(sessions)}", file=sys.stderr)
        scores.append(_score(domain, path))

    buckets = _pick_buckets(scores)
    if not buckets:
        print("no buckets populated (no sessions met minima)",
              file=sys.stderr)
        return 1

    print(f"\nselected {len(buckets)} bucket(s):")
    for b, s in buckets.items():
        c = s.case
        redacted_n = _verify_redaction(c) if c else 0
        print(f"  {b:<20} {s.domain:<7} events={s.n_events:>5} "
              f"tools={s.n_tool_calls:>4} corrections={s.n_corrections:>3} "
              f"compaction={s.has_compaction} "
              f"redacted_events={redacted_n}/{len(c.events)}")

    if args.dry_run:
        return 0

    args.out_dir.mkdir(parents=True, exist_ok=True)
    for b, s in buckets.items():
        if s.case is None:
            continue
        out_path = args.out_dir / _bucket_filename(b)
        _write_case_json(s.case, out_path)
        print(f"  wrote {out_path}")
    _write_index(buckets, args.out_dir,
                 generated_at=time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                             time.gmtime()))
    print(f"  wrote {args.out_dir / 'index.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
