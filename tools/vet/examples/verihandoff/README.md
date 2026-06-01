# VeriHandoff

**VeriHandoff** is the worked example for VET verification. It shows how a third party can check that a JSON handoff bundle still matches the live append-only log and the session's **Agent Identity Document (AID)**.

Think of it like a receipt check: the handoff says "here is what I remember," and `vet verify` checks that receipt against the actual log file on disk.

## What is included

| File or folder | Purpose |
|----------------|---------|
| `run_demo.sh` | Full demo in a temporary folder: record, handoff, verify, tamper, verify again |
| `verify.html` | Browser page to read verify results without parsing JSON |
| `fixtures/golden/` | Saved session + handoff + expected verify output for automated tests |
| `verify_golden.sh` | Script that checks the golden fixtures still verify |
| `regenerate_golden.sh` | Rebuild golden files after intentional schema changes |

Continuous integration (CI) runs these checks in [`.github/workflows/verihandoff.yml`](../../../.github/workflows/verihandoff.yml).

## What verification checks

- session identity (`tenant_id` / `session_id`)
- AID digest (`aid.json` unchanged since handoff)
- BLAKE3 trace digest over `events.dpmlog`
- event count and event range
- correction count in bundle vs live log
- structural validity of the log (order, identities on each event)

## What verification does not check

Listed in `aid.json` under `claims.does_not_verify`:

- host orchestration (prompt wiring, omitted events)
- language model (LLM) API calls
- tool HTTP transcripts

## Run the live demo

```sh
bazelisk build --config=vet_release_no_android //tools/vet:vet
chmod +x tools/vet/examples/verihandoff/run_demo.sh
tools/vet/examples/verihandoff/run_demo.sh
```

Custom binary:

```sh
VET_BIN=/path/to/vet tools/vet/examples/verihandoff/run_demo.sh
```

Steps inside the script:

1. Initialize a fresh session
2. Record a user fact and a correction
3. Write a JSON handoff bundle
4. Verify — expect `"verified": true`
5. Tamper with the log
6. Verify again — expect failure on `trace_digest_match`

## Verifier UI

Open in a browser:

```sh
open tools/vet/examples/verihandoff/verify.html
```

Load `sample-verify-pass.json`, `fixtures/golden/verify-pass.json`, or your own output from `vet verify --json`.

Reference screenshot: `verify-ui-screenshot.png`.

The page **displays** verify reports for humans. It does not re-run checks. Run `vet verify` in the terminal first.

## Golden fixtures

```sh
tools/vet/examples/verihandoff/verify_golden.sh
```

After you change handoff or verify behavior on purpose:

```sh
tools/vet/examples/verihandoff/regenerate_golden.sh
```

See [`fixtures/README.md`](./fixtures/README.md).

## Manual walkthrough

```sh
vet init --root .vet --tenant local --session my-session
vet record --type user --payload "Important constraint"
vet correction --text "Fix metric" \
  --invalidated-fact "old fact" \
  --replacement-fact "new fact"
vet handoff --task "Continue work" --format json --out handoff.json
vet verify --bundle handoff.json --json
open tools/vet/examples/verihandoff/verify.html
```

Example failure output:

```json
{
  "verified": false,
  "checks_failed": ["trace_digest_match"],
  "failure_details": {
    "trace_digest_match": "bundle trace_digest abc... != live trace_digest def..."
  }
}
```
