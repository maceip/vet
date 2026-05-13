# DPM Claude Gate Demo

This directory wires Claude Code hooks to a small DPM gate-mode demo.

Runtime state is written to `.dpm-agent-hooks/` and ignored by git. The shared
hook script records prompt/tool events, detects user corrections, injects a
generated verified-context patch, and blocks action-boundary tool calls that
still carry invalidated facts.

Useful commands:

```powershell
python tools/agent_hooks/dpm_gate.py --agent claude --reset
python tools/agent_hooks/dpm_gate.py --agent claude --demo-seed
python tools/agent_hooks/dpm_gate.py --agent claude --status
```

For the seeded demo, ask Claude to write or commit something containing
`transport as the main result`. The `PreToolUse` hook should deny that action
and point Claude at `.dpm-agent-hooks/ACTIVE_CONTEXT.md`.
