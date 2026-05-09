# DPM Gate Case

Case: `correction-heavy-session`

Condition: `dpm_phase3_checkpoint`

Budget: `1338` chars

Gate may use checkpoint: `False`

Audit verdict: `correction_emitted`

Audit certificate id: `dc8c9ec5c954296f544e89b0712784888ad32ae5f009bab1cee09716efbea38d`

Checkpoint manifest hash: `15213e738125e2a18704582636fb3f8f52ecee9b9f8ba00246457eb96d320d31`

Blocking corrections: `585f87c25cc5213af65dde4a2ff25f7fc586b5c7992ed57fc78237491dda57e2`

Gate reason:

blocking correction invalidates checkpoint; re-projected from raw event range

Why this matters:

DPM exposes the operational evidence rolling memory cannot: which checkpoint was
eligible for decision memory, what the auditor concluded, and whether a blocking
correction forced the system to fail closed or reproject.

Notes:

all deterministic checks passed
