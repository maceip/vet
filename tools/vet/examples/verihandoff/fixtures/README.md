# Golden fixtures for VeriHandoff

This folder holds a **saved session** used by automated tests. The files are checked into git so continuous integration (CI) can run `vet verify` and expect the same result every time.

## Files

| Path | Meaning |
|------|---------|
| `golden/.vet/.../events.dpmlog` | Append-only event log for the golden session |
| `golden/.vet/.../aid.json` | Agent Identity Document (AID) for that session |
| `golden/handoff.json` | JSON handoff bundle tied to that log |
| `golden/verify-pass.json` | Expected successful verify report |

## Regenerate after intentional changes

If you change handoff JSON shape, verify checks, or log format on purpose:

```sh
bazelisk build --config=vet_release_no_android //tools/vet:vet
tools/vet/examples/verihandoff/regenerate_golden.sh
```

Then commit the updated `golden/` files and run:

```sh
tools/vet/examples/verihandoff/verify_golden.sh
```
