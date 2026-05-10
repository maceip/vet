# DPM Gate Case

Case: `correction-heavy-session`

Condition: `dpm_phase3_checkpoint`

Budget: `1338` chars

Gate may use checkpoint: `False`

Audit verdict: `correction_emitted`

Audit certificate id: `e08834e1372ae57c8c3985e5e366f8d4d1a1e419c10dd30dcee6e7317c804d17`

Checkpoint manifest hash: `02d4dced6fa215da45b8ae448d4bfa961f001d15feee903f6c0f28bf2e7ff081`

Blocking corrections: `585f87c25cc5213af65dde4a2ff25f7fc586b5c7992ed57fc78237491dda57e2`

Gate reason:

blocking correction invalidates checkpoint; re-projected from raw event range

Why this matters:

DPM exposes the operational evidence rolling memory cannot: which checkpoint was
eligible for decision memory, what the auditor concluded, and whether a blocking
correction forced the system to fail closed or reproject.

Notes:

all deterministic checks passed
