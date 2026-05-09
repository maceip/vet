"""Memory-agent contract.

Engineers 3 (baselines) and 4 (DPM substrate) implement concrete agents
in `memory_agents.py`. The runner imports the registry from there. This
module declares only the contract: `MemoryAgent` protocol +
`AgentResult` dataclass, so all four lanes agree on the shape without
locking implementations.

Every agent run produces an `AgentResult`. The runner converts the
result into a `BenchRow` together with the scorer's output.

Per-condition required fields:

  raw_oracle:
    answer_bytes, model_calls (=0 if skipped, =1 if it ran),
    input_tokens, output_tokens, wall_ms, answer_sha256.
    score_status=skipped_context_too_large is allowed when the full log
    doesn't fit; in that case decision_score must remain None.

  rolling_summary:
    memory_bytes, answer_bytes, model_calls (= summarize calls + 1 for
    the decision), tokens, wall_ms, memory_sha256, answer_sha256.
    audit fields stay empty.

  dpm_phase3_checkpoint:
    All of the above PLUS the substrate evidence:
      checkpoint_manifest_hash, checkpoint_body_hash,
      audit_certificate_id, audit_verdict, drift_score,
      gate_may_use, gate_reason, blocking_corrections,
      log_generation, event_range_start, event_range_end,
      projection_model_id, auditor_model_id, audit_policy_version.
    If gate_may_use=False the row is still valid as long as gate_reason
    is non-empty; answer_bytes and decision_score should reflect the
    fail-closed path (e.g., re-projected from raw or refused outright).
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Protocol, runtime_checkable

try:
    from tools.benchmarks.dpm_phase3_bench.bench_schema import (
        AuditVerdict,
        Condition,
        ScoreStatus,
    )
    from tools.benchmarks.dpm_phase3_bench.session_case import (
        SessionCase,
        SessionProbe,
    )
except ModuleNotFoundError:  # Allows running from this directory directly.
    from bench_schema import AuditVerdict, Condition, ScoreStatus  # type: ignore
    from session_case import SessionCase, SessionProbe  # type: ignore


@dataclass
class AgentResult:
    """Output of a single agent run on one (case, probe, budget)."""

    # ---- universal fields ----
    condition: Condition
    memory_bytes: str = ""
    answer_bytes: str = ""

    model_calls: int = 0
    input_tokens: int = 0
    output_tokens: int = 0
    wall_ms: int = 0

    memory_sha256: str = ""
    answer_sha256: str = ""

    # If the agent decided not to run (e.g., raw_oracle context too
    # large), it sets a non-default score_status here. The runner uses
    # this to keep the row valid and excluded from aggregates.
    score_status_override: ScoreStatus | None = None

    # Free-form note (raw oracle skip reason, agent-side error, etc.)
    notes: str = ""

    # ---- DPM-only fields (empty / None on rolling_summary / raw_oracle) ----
    projection_model_id: str = ""
    auditor_model_id: str = ""
    audit_policy_version: str = ""

    event_range_start: int = 0
    event_range_end: int = 0
    log_generation: int = 0

    checkpoint_manifest_hash: str = ""
    checkpoint_body_hash: str = ""
    audit_certificate_id: str = ""
    audit_verdict: AuditVerdict = AuditVerdict.NOT_APPLICABLE
    drift_score: float | None = None
    gate_may_use: bool | None = None
    gate_reason: str = ""
    blocking_corrections: list[str] = field(default_factory=list)


@runtime_checkable
class MemoryAgent(Protocol):
    """All agents implement this single method.

    `task` is the natural-language probe question; the agent should use
    it to condition its rebuild and produce an answer. `budget_chars`
    is the memory budget the agent must respect (rolling and DPM both
    clip to this). The runner passes the SAME final-decision prompt
    shape across conditions; that's enforced upstream of this method.
    """

    @property
    def condition(self) -> Condition: ...

    def run(
        self,
        case: SessionCase,
        probe: SessionProbe,
        budget_chars: int,
    ) -> AgentResult: ...


# Engineers 3 and 4 populate this in memory_agents.py:
#
#   from agent_protocol import MemoryAgent
#   AGENT_REGISTRY: dict[Condition, type[MemoryAgent]] = {
#       Condition.RAW_ORACLE: RawOracleAgent,
#       Condition.ROLLING_SUMMARY: RollingSummaryAgent,
#       Condition.DPM_PHASE3_CHECKPOINT: DpmPhase3CheckpointAgent,
#   }
#
# The runner imports memory_agents.AGENT_REGISTRY at startup and fails
# loudly if any expected condition is missing.
