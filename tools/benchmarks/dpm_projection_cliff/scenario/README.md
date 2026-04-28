# Scenario corpus from real agent sessions

This directory turns real Claude Code / Codex session logs into
`SessionCase` records the property + scenario tests can run against.
The whole point: the substrate's "does it work for actual users?"
question is answered by replaying real sessions, not synthetic ones.

## Quick reference

| File | Role |
|---|---|
| `redactor.py` | Deterministic, length-stable credential redaction. Self-tests with `python redactor.py`. |
| `ingest_session.py` | Reads one Claude or Codex session JSONL, normalizes events, redacts, picks probe-points, derives ground-truth, emits `SessionCase` JSON. |

## Probe kinds we extract

Each session yields N case records, one per probe-point T. At each T,
we emit one or more probes that map onto property→scenario tests:

| Probe kind | Property it scenarios | Ground-truth source |
|---|---|---|
| `next_user_intent` | replay determinism, range coverage | substring of the next user turn after T |
| `next_tool_call` | replay determinism, replay-from-raw | tool name + first arg of the next assistant tool_use after T |
| `correction_detection` | merkle integrity, replay-from-raw | the user's own correction message at T |

Probe-points are auto-selected:
1. Three fixed positions (10%, 50%, 80% of session length).
2. Plus any user turn whose text contains a correction keyword
   (`"you drifted"`, `"wait wait"`, `"actually use"`, etc.).

## Format support

- **Claude** sessions (`.claude/projects/<dir>/<uuid>.jsonl`): top-level
  `{type, message{content[]}}` records. Tool-use blocks inside assistant
  content are flattened to `tool_call` events. Sub-agent and `local-
  command-caveat` user messages are filtered out.
- **Codex** sessions (`.codex/sessions/<year>/<month>/<day>/rollout-*.jsonl`):
  per-line `{type, timestamp, payload}` envelopes; `payload.type` of
  `message`, `function_call`, and `function_call_output` map to our
  user/assistant/tool_call/tool_result events.

## Redaction guarantees

- Deterministic: same secret → same placeholder, every time.
- Length-stable: placeholder length matches the original (±0).
- Audit trail: each redaction records the secret's sha256[:8] so we
  can answer "what kind of secret was at offset N" without keeping it.

Run `python redactor.py` to verify the patterns hit on canonical
samples and don't false-positive on common code.

## Usage

```bash
# Summary mode — counts events, lists probe-points.
python ingest_session.py path/to/session.jsonl --summary

# Full JSON output for the test fixture pipeline.
python ingest_session.py path/to/session.jsonl > case.json

# Override the case_id (default is the session uuid prefix).
python ingest_session.py path/to/session.jsonl --case-id my-case
```

## How the engineer uses this

The `SessionCase` JSON is consumed by the property + scenario test
fixture (planned, not yet committed) at
`runtime/platform/checkpoint/phase2_substrate_property_test.cc`. Each
property test gets a twin scenario test parameterized on a
`SessionCase`; running the fixture against `[paper-corpus, real-sessions]`
gives both academic-credibility signal (paper reproduction) and
real-world signal (does this help your actual workflows).

Plug a session into the pipeline by:

```python
from ingest_session import parse_claude_session, make_session_case
from pathlib import Path

p = Path("path/to/session.jsonl")
events, sha = parse_claude_session(p)
cases = make_session_case(events, p, sha, "my-session", "claude")
# cases is now a list[SessionCase] you can serialize per fixture.
```

## Known limitations

- Codex parser is best-effort against the OpenAI Responses API event
  shape; if Codex format drifts, this needs a refresh.
- Probe-point selection is heuristic. If a session contains a
  significant correction we miss (no keyword match), the probe doesn't
  fire. Fix: add the phrase to `CORRECTION_KEYWORDS`.
- Ground truth for `next_user_intent` is a single substring; some
  sessions have user turns that cleanly continue but with paraphrased
  shape, and a strict substring match won't capture them. Future work:
  judge-scored variant.
- Probe-points beyond 80% of session length are skipped to leave room
  for at least 5 events of ground-truth lookahead.
