# DPM Gate Case

Case: `correction-heavy-session`

Condition: `dpm_phase3_checkpoint`

Budget: `1338` chars

Gate may use checkpoint: `False`

Audit verdict: `correction_emitted`

Manifest fingerprint (Python SHA-256, mirrors substrate cert semantics): `cd834b5781583494cd84b653d56ff2a8493f683cc670a8d0f388773e164f7f6e`

Checkpoint manifest hash (Python SHA-256): `5da321c75777a90dd53762a4daefc15b6ad504d3cbbef728931512a9b367c48d`

Blocking corrections: `585f87c25cc5213af65dde4a2ff25f7fc586b5c7992ed57fc78237491dda57e2`

Gate reason:

blocking correction invalidates checkpoint; re-projected from raw event range

Why this matters:

DPM exposes the operational evidence rolling memory cannot: which checkpoint was
eligible for decision memory, what the auditor concluded, and whether a blocking
correction forced the system to fail closed or reproject.

Notes:

must_include missing: 'checkpointed projection is the contribution'
