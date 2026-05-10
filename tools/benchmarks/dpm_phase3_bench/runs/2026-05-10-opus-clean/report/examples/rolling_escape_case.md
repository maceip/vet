# Rolling Memory Escape Case

Case: `correction-heavy-session`

Condition: `rolling_summary`

Budget: `1338` chars

Decision score: `1.0`

Stale-memory escape: `True`

Why this matters:

Rolling memory produced a plausible final summary, but the row was marked as a
stale-memory escape. In Phase 3 terms, this is the failure mode DPM is designed
to prevent: there is no checkpoint certificate, event range, or correction gate
that can revoke the stale compressed state before the next decision.

Notes:

all deterministic checks passed
