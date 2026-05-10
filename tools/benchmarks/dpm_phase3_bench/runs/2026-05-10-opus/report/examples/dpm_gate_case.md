# DPM Gate Case

Case: `correction-heavy-session`

Condition: `dpm_phase3_checkpoint`

Budget: `1338` chars

Gate may use checkpoint: `False`

Audit verdict: `correction_emitted`

Audit certificate id: `f304ca4d66e7038d8b6316cfa4b775da30e2e410be90716e89c4a4d059fac048`

Checkpoint manifest hash: `3f647a93f356bef0f94e9999e7ff53070557bb46b659e58f2a4c1a19dbbeb3f2`

Blocking corrections: `585f87c25cc5213af65dde4a2ff25f7fc586b5c7992ed57fc78237491dda57e2`

Gate reason:

blocking correction invalidates checkpoint; re-projected from raw event range

Why this matters:

DPM exposes the operational evidence rolling memory cannot: which checkpoint was
eligible for decision memory, what the auditor concluded, and whether a blocking
correction forced the system to fail closed or reproject.

Notes:

must_include missing: 'checkpointed projection is the contribution'
