# DPM Gate Case

Case: `judged-unredacted-claude-correction-04`

Condition: `dpm_phase3_checkpoint`

Budget: `669` chars

Gate may use checkpoint: `False`

Audit verdict: `correction_emitted`

Manifest fingerprint (Python SHA-256, mirrors substrate cert semantics): `8289b0f0f89ee472b4610825301cfd6e2c183fe65cda75944ec8184e8bb30837`

Checkpoint manifest hash (Python SHA-256): `0b4f683f062feb3a69d3c3470ef6f0dbc2784ccac5e17b818ac4aa5fdb062f01`

Blocking corrections: `b6463d81f08a22b8d4f2cb2123ad3fff58362f92a624bb66415ed3cbe01a64f8`

Gate reason:

blocking correction invalidates checkpoint; re-projected from raw event range

Why this matters:

DPM exposes the operational evidence rolling memory cannot: which checkpoint was
eligible for decision memory, what the auditor concluded, and whether a blocking
correction forced the system to fail closed or reproject.

Notes:

must_include missing: 'reader and writer must match record size rules'
