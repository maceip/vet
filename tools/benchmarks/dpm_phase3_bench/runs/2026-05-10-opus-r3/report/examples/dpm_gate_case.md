# DPM Gate Case

Case: `correction-heavy-session`

Condition: `dpm_phase3_checkpoint`

Budget: `1338` chars

Gate may use checkpoint: `False`

Audit verdict: `correction_emitted`

Audit certificate id: `cc32982e3e3e5961e187cd224b59176ad11b4ff2a980d1b0e041b510ef8b2572`

Checkpoint manifest hash: `9a8802f02be763b37a70db8bbe42454a1d6c7523df2c2c9fc38cf64725e8840f`

Blocking corrections: `585f87c25cc5213af65dde4a2ff25f7fc586b5c7992ed57fc78237491dda57e2`

Gate reason:

blocking correction invalidates checkpoint; re-projected from raw event range

Why this matters:

DPM exposes the operational evidence rolling memory cannot: which checkpoint was
eligible for decision memory, what the auditor concluded, and whether a blocking
correction forced the system to fail closed or reproject.

Notes:

all deterministic checks passed
