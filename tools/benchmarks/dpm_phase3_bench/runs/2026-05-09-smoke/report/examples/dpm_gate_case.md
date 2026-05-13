# DPM Gate Case

Case: `correction-heavy-session`

Condition: `dpm_phase3_checkpoint`

Budget: `1338` chars

Gate may use checkpoint: `False`

Audit verdict: `correction_emitted`

Audit certificate id: `5009bd415af9be382bff60fd351ef79463803077e61a86500ec4c9853ae29e84`

Checkpoint manifest hash: `88fa9bafc55b4998135dfd528f01e16f432fc9c1d1b4ee6c4bef6975533bebbb`

Blocking corrections: `585f87c25cc5213af65dde4a2ff25f7fc586b5c7992ed57fc78237491dda57e2`

Gate reason:

blocking correction invalidates checkpoint; re-projected from raw event range

Why this matters:

DPM exposes the operational evidence rolling memory cannot: which checkpoint was
eligible for decision memory, what the auditor concluded, and whether a blocking
correction forced the system to fail closed or reproject.

Notes:

all deterministic checks passed
