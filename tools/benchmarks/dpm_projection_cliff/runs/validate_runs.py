"""Validate ScoreRow JSONL files against the locked score_schema.

Usage:
  python runs/validate_runs.py [path1.jsonl] [path2.jsonl ...]
  python runs/validate_runs.py            # validates everything in runs/

Exits non-zero on any schema violation. Intended as a pre-publish guard
and as a CI hook target. The whole point of the schema is that no
chart-targetable JSONL can sneak through with a category error
(e.g. memory_projection scored from prompt_bytes); this runner is the
gate.

What it checks per row:
  - row deserializes via ScoreRow.from_dict (which calls validate_row)
  - bytes_sha256 is 64 hex chars
  - bytes_len matches the implied content (when bytes_len is positive,
    just enforces non-negative; we don't have the raw bytes here so
    can't recompute the sha)
  - case_id and case_corpus are non-empty
  - if pair_role is set, paired_case_id must also be set, and vice
    versa

What it does NOT check:
  - whether the model_id was actually called (no API state to verify)
  - whether the scores dict carries any particular keys (each runner
    chooses its own; the schema is bytes-and-substrate, not metric)
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scenario"))
from score_schema import ScoreRow, SchemaError  # noqa: E402

_HEX64 = re.compile(r"^[0-9a-f]{64}$")


def _validate_one(path: Path) -> tuple[int, list[str]]:
    errors: list[str] = []
    n_ok = 0
    with path.open("r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, start=1):
            line = raw.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except Exception as e:
                errors.append(f"{path}:{lineno}: bad json: {e}")
                continue
            try:
                row = ScoreRow.from_dict(d)
            except SchemaError as e:
                errors.append(f"{path}:{lineno}: schema: {e}")
                continue
            except Exception as e:
                errors.append(f"{path}:{lineno}: row: {type(e).__name__}: {e}")
                continue
            if not _HEX64.match(row.bytes_sha256):
                errors.append(
                    f"{path}:{lineno}: bytes_sha256 not 64 hex chars: "
                    f"{row.bytes_sha256!r}")
                continue
            if not row.case_id or not row.case_corpus:
                errors.append(
                    f"{path}:{lineno}: case_id/case_corpus must be non-empty")
                continue
            if bool(row.paired_case_id) != bool(row.pair_role):
                errors.append(
                    f"{path}:{lineno}: paired_case_id and pair_role must "
                    "be set together (both present or both empty)")
                continue
            n_ok += 1
    return n_ok, errors


def main(argv: list[str]) -> int:
    if argv:
        targets = [Path(p) for p in argv]
    else:
        runs_dir = Path(__file__).resolve().parent
        targets = sorted(runs_dir.glob("*.jsonl"))
    if not targets:
        print("no .jsonl files to validate", file=sys.stderr)
        return 0
    total_ok = 0
    total_err = 0
    for p in targets:
        if not p.exists():
            print(f"missing: {p}", file=sys.stderr)
            total_err += 1
            continue
        n_ok, errs = _validate_one(p)
        total_ok += n_ok
        total_err += len(errs)
        status = "OK" if not errs else "FAIL"
        print(f"  {status} {p} ({n_ok} rows ok, {len(errs)} errors)")
        for e in errs:
            print(f"    {e}", file=sys.stderr)
    print(f"\ntotal: {total_ok} rows ok, {total_err} errors across "
          f"{len(targets)} file(s)")
    return 1 if total_err else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
