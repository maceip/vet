"""Locked schema for differential-scoring rows + a chart-side guard.

Why this exists: it's easy to write a runner that scores `prompt_bytes`
against rubric probes and call the result a "DPM differential." That
result is fake — the raw evidence is in the prompt either way, so DPM
appears artificially great. The guard in this module makes it
*impossible* to plot prompt-byte assertions on a DPM-vs-baseline panel
without an explicit, recorded category override.

Three discriminators on every row:

  test_kind          ∈ {prompt_retention, memory_projection,
                        decision, judge_scored_decision}
  compression_substrate ∈ {raw_full_context, dpm_projection,
                           rolling_summary, checkpointed_dpm}
  bytes_scored_from  ∈ {prompt_bytes, memory_bytes, answer_bytes}

Validator rules:

  - prompt_retention      MUST score from prompt_bytes
  - memory_projection     MUST score from memory_bytes
  - decision              MUST score from answer_bytes
  - judge_scored_decision MUST score from answer_bytes

  - raw_full_context substrate cannot produce memory_projection
    rows (no compressor produced a memory artifact).

These rules cover the trap the user named: a memory_projection row
backed by prompt_bytes is rejected at row-construction time.

Public API:

  ScoreRow(case_id=..., compression_substrate=..., test_kind=...,
           bytes_scored_from=..., bytes=..., scores={...},
           model_id=..., budget_chars=...)
  ScoreRow.from_dict(dict_row) -> ScoreRow (validates)
  validate_row(row) -> raises SchemaError on violation
  ChartSpec(name, allowed_test_kinds, allowed_substrates).filter(rows)
      -> raises SchemaError if rows mix incompatible kinds.

A "DPM-vs-rolling-summary" chart instantiates ChartSpec with
allowed_test_kinds={memory_projection, decision, judge_scored_decision}
and allowed_substrates={dpm_projection, rolling_summary,
checkpointed_dpm}, which by construction cannot accept a
prompt_retention row.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field, asdict
from enum import Enum
from typing import Iterable


class TestKind(str, Enum):
    PROMPT_RETENTION = "prompt_retention"
    MEMORY_PROJECTION = "memory_projection"
    DECISION = "decision"
    JUDGE_SCORED_DECISION = "judge_scored_decision"


class CompressionSubstrate(str, Enum):
    RAW_FULL_CONTEXT = "raw_full_context"
    DPM_PROJECTION = "dpm_projection"
    ROLLING_SUMMARY = "rolling_summary"
    CHECKPOINTED_DPM = "checkpointed_dpm"


class BytesScoredFrom(str, Enum):
    PROMPT_BYTES = "prompt_bytes"
    MEMORY_BYTES = "memory_bytes"
    ANSWER_BYTES = "answer_bytes"


# Test-kind → required bytes_scored_from. Anything else is a category
# error and the validator refuses it.
_BYTES_FOR_KIND = {
    TestKind.PROMPT_RETENTION: BytesScoredFrom.PROMPT_BYTES,
    TestKind.MEMORY_PROJECTION: BytesScoredFrom.MEMORY_BYTES,
    TestKind.DECISION: BytesScoredFrom.ANSWER_BYTES,
    TestKind.JUDGE_SCORED_DECISION: BytesScoredFrom.ANSWER_BYTES,
}


class SchemaError(ValueError):
    """Raised when a ScoreRow or chart filter violates the contract."""


@dataclass
class ScoreRow:
    case_id: str
    case_corpus: str           # "paper" | "agentic_qwen" | "real_sessions" | "synthetic"
    compression_substrate: CompressionSubstrate
    budget_chars: int
    test_kind: TestKind
    bytes_scored_from: BytesScoredFrom
    bytes_len: int             # length of the bytes that were scored
    bytes_sha256: str          # hash of the scored bytes (audit)
    model_id: str              # the LLM that produced the bytes (or "" for raw)
    judge_model_id: str = ""   # set only when test_kind == judge_scored_decision
    seed: int = 20260420
    scores: dict = field(default_factory=dict)
    # Optional pair linkage — populated when this row is one half of a
    # twin-differential (e.g. AgenticQwen normal vs hack).
    paired_case_id: str = ""
    pair_role: str = ""        # "normal" | "hack" | ""
    # Free-form note if the runner had to record an unusual condition
    # (e.g. truncation, retry, partial hit). Empty in normal rows.
    note: str = ""

    def __post_init__(self) -> None:
        # Coerce string inputs to enums so callers don't have to.
        if isinstance(self.compression_substrate, str):
            self.compression_substrate = CompressionSubstrate(self.compression_substrate)
        if isinstance(self.test_kind, str):
            self.test_kind = TestKind(self.test_kind)
        if isinstance(self.bytes_scored_from, str):
            self.bytes_scored_from = BytesScoredFrom(self.bytes_scored_from)
        validate_row(self)

    def to_dict(self) -> dict:
        d = asdict(self)
        # Enum -> str so JSONL is portable.
        for k in ("compression_substrate", "test_kind", "bytes_scored_from"):
            v = d.get(k)
            d[k] = v.value if isinstance(v, Enum) else v
        return d

    @classmethod
    def from_dict(cls, src: dict) -> "ScoreRow":
        return cls(**{k: src[k] for k in src if k in cls.__dataclass_fields__})


def validate_row(row: ScoreRow) -> None:
    """Refuse fake-DPM-differential rows by construction.

    The single rule that catches the trap: bytes_scored_from must
    match what test_kind logically operates on. A memory_projection
    test scoring prompt_bytes is a category error.
    """
    expected = _BYTES_FOR_KIND.get(row.test_kind)
    if expected is None:
        raise SchemaError(f"unknown test_kind: {row.test_kind!r}")
    if row.bytes_scored_from != expected:
        raise SchemaError(
            f"category error: test_kind={row.test_kind.value} requires "
            f"bytes_scored_from={expected.value}, got "
            f"{row.bytes_scored_from.value}. This is the exact "
            f"DPM-vs-rolling-summary trap; refusing to record this row.")
    if (row.compression_substrate == CompressionSubstrate.RAW_FULL_CONTEXT
            and row.test_kind == TestKind.MEMORY_PROJECTION):
        raise SchemaError(
            "raw_full_context substrate cannot produce a "
            "memory_projection row: there is no compressor that emits "
            "a memory artifact. Use prompt_retention or decision.")
    if (row.test_kind == TestKind.JUDGE_SCORED_DECISION
            and not row.judge_model_id):
        raise SchemaError(
            "test_kind=judge_scored_decision requires judge_model_id")
    if row.budget_chars <= 0:
        raise SchemaError(
            f"budget_chars must be positive, got {row.budget_chars}")
    if row.bytes_len < 0:
        raise SchemaError(
            f"bytes_len must be non-negative, got {row.bytes_len}")


@dataclass(frozen=True)
class ChartSpec:
    """Declares what test_kind and substrate set a chart accepts.

    The differential chart "DPM vs rolling-summary" constructs a
    ChartSpec that excludes prompt_retention and raw_full_context.
    Passing a row that doesn't match raises SchemaError; chart code
    cannot silently downgrade a real-DPM panel to a prompt-bytes
    panel.
    """
    name: str
    allowed_test_kinds: frozenset[TestKind]
    allowed_substrates: frozenset[CompressionSubstrate]

    def filter(self, rows: Iterable[ScoreRow]) -> list[ScoreRow]:
        out: list[ScoreRow] = []
        for r in rows:
            if r.test_kind not in self.allowed_test_kinds:
                raise SchemaError(
                    f"chart {self.name!r}: rejected test_kind="
                    f"{r.test_kind.value} (case {r.case_id})")
            if r.compression_substrate not in self.allowed_substrates:
                raise SchemaError(
                    f"chart {self.name!r}: rejected substrate="
                    f"{r.compression_substrate.value} (case {r.case_id})")
            out.append(r)
        return out


# Canonical chart specs the headline panels use. Builders must use
# these by reference, not by reconstructing equivalent ones inline.

DPM_VS_ROLLING_SUMMARY_DIFFERENTIAL = ChartSpec(
    name="dpm_vs_rolling_summary_differential",
    allowed_test_kinds=frozenset({
        TestKind.MEMORY_PROJECTION,
        TestKind.DECISION,
        TestKind.JUDGE_SCORED_DECISION,
    }),
    allowed_substrates=frozenset({
        CompressionSubstrate.DPM_PROJECTION,
        CompressionSubstrate.ROLLING_SUMMARY,
        CompressionSubstrate.CHECKPOINTED_DPM,
    }),
)

PROMPT_RETENTION_DIAGNOSTICS = ChartSpec(
    name="prompt_retention_diagnostics",
    allowed_test_kinds=frozenset({TestKind.PROMPT_RETENTION}),
    allowed_substrates=frozenset({
        CompressionSubstrate.RAW_FULL_CONTEXT,
        CompressionSubstrate.DPM_PROJECTION,
        CompressionSubstrate.ROLLING_SUMMARY,
        CompressionSubstrate.CHECKPOINTED_DPM,
    }),
)


def _selftest() -> int:
    """Smoke. Run with `python score_schema.py`."""
    fails = 0

    def expect_ok(builder, label):
        nonlocal fails
        try:
            r = builder()
            assert isinstance(r, ScoreRow)
        except Exception as e:
            print(f"FAIL ok-case {label}: {e}")
            fails += 1

    def expect_err(builder, label):
        nonlocal fails
        try:
            builder()
            print(f"FAIL err-case {label}: expected SchemaError, got OK")
            fails += 1
        except SchemaError:
            pass
        except Exception as e:
            print(f"FAIL err-case {label}: wrong exception {type(e).__name__}: {e}")
            fails += 1

    base = dict(case_id="c1", case_corpus="real_sessions", budget_chars=1338,
                bytes_len=100, bytes_sha256="0"*64, model_id="m")
    expect_ok(lambda: ScoreRow(**base, compression_substrate="dpm_projection",
                                test_kind="memory_projection",
                                bytes_scored_from="memory_bytes"),
              "dpm/memory_projection/memory_bytes")
    expect_ok(lambda: ScoreRow(**base, compression_substrate="rolling_summary",
                                test_kind="decision",
                                bytes_scored_from="answer_bytes"),
              "rolling/decision/answer_bytes")
    expect_ok(lambda: ScoreRow(**base, compression_substrate="raw_full_context",
                                test_kind="prompt_retention",
                                bytes_scored_from="prompt_bytes"),
              "raw/prompt_retention/prompt_bytes")
    expect_ok(lambda: ScoreRow(**base, compression_substrate="dpm_projection",
                                test_kind="judge_scored_decision",
                                bytes_scored_from="answer_bytes",
                                judge_model_id="judge-1"),
              "dpm/judge/answer_bytes")

    # The trap: memory_projection row backed by prompt_bytes.
    expect_err(lambda: ScoreRow(**base, compression_substrate="dpm_projection",
                                 test_kind="memory_projection",
                                 bytes_scored_from="prompt_bytes"),
               "trap: memory_projection + prompt_bytes")
    # decision row backed by memory_bytes is also wrong.
    expect_err(lambda: ScoreRow(**base, compression_substrate="dpm_projection",
                                 test_kind="decision",
                                 bytes_scored_from="memory_bytes"),
               "decision + memory_bytes")
    # raw_full_context cannot produce memory_projection.
    expect_err(lambda: ScoreRow(**base, compression_substrate="raw_full_context",
                                 test_kind="memory_projection",
                                 bytes_scored_from="memory_bytes"),
               "raw + memory_projection")
    # judge_scored_decision without a judge_model_id.
    expect_err(lambda: ScoreRow(**base, compression_substrate="dpm_projection",
                                 test_kind="judge_scored_decision",
                                 bytes_scored_from="answer_bytes"),
               "judge_scored_decision missing judge_model_id")

    # Chart guard: a DPM-vs-rolling chart must reject prompt_retention rows.
    good = ScoreRow(**base, compression_substrate="dpm_projection",
                     test_kind="memory_projection",
                     bytes_scored_from="memory_bytes")
    bad = ScoreRow(**base, compression_substrate="dpm_projection",
                    test_kind="prompt_retention",
                    bytes_scored_from="prompt_bytes")
    try:
        DPM_VS_ROLLING_SUMMARY_DIFFERENTIAL.filter([good])
    except Exception as e:
        print(f"FAIL chart-good: {e}"); fails += 1
    try:
        DPM_VS_ROLLING_SUMMARY_DIFFERENTIAL.filter([good, bad])
        print("FAIL chart-bad: chart accepted a prompt_retention row")
        fails += 1
    except SchemaError:
        pass

    # Chart guard: prompt-retention chart must reject memory_projection rows.
    try:
        PROMPT_RETENTION_DIAGNOSTICS.filter([good])
        print("FAIL chart-prompt: prompt-retention chart accepted memory_projection")
        fails += 1
    except SchemaError:
        pass

    # JSON round-trip: to_dict + from_dict produces an equivalent row.
    d = good.to_dict()
    rt = ScoreRow.from_dict(d)
    if rt.test_kind != good.test_kind \
            or rt.bytes_scored_from != good.bytes_scored_from \
            or rt.compression_substrate != good.compression_substrate:
        print("FAIL json-roundtrip"); fails += 1

    if fails == 0:
        print("score_schema self-test: ALL PASS")
    else:
        print(f"score_schema self-test: {fails} FAILURES")
    return fails


if __name__ == "__main__":
    raise SystemExit(_selftest())
