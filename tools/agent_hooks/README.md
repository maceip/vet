# Agent hooks (demo)

This folder contains a shared **gate** script for local coding agents. The gate runs before tool use and can block or patch actions when **correction** events say the pending action would reuse stale facts.

Normal rolling chat memory stays in place. The gate adds a check at action boundaries.

Runtime state is written to `.dpm-agent-hooks/` and ignored by git.

## Supported agents

| Agent | Config file |
|-------|-------------|
| Claude Code | `.claude/settings.json` |
| Codex CLI | `.codex/config.toml` |
| Gemini CLI | `.gemini/settings.json` |
| Cursor Agent | `.cursor/hooks.json` |
| GitHub Copilot | `.github/hooks/dpm-gate.json` |

## Smoke commands

```sh
python tools/agent_hooks/dpm_gate.py --agent claude --reset
python tools/agent_hooks/dpm_gate.py --agent claude --demo-seed
python tools/agent_hooks/dpm_gate.py --agent claude --status
```

Synthetic deny example (PowerShell):

```powershell
'{"hook_event_name":"PreToolUse","tool_name":"Write","tool_input":{"content":"transport as the main result"}}' |
  python tools/agent_hooks/dpm_gate.py --agent claude
```

Expected: a deny response pointing at `.dpm-agent-hooks/ACTIVE_CONTEXT.md`.

## Cursor fallback wrapper

If native hooks are not firing in a headless test:

```sh
python tools/agent_hooks/cursor_agent_gate.py --prompt "make a harmless change"
```

## Codex user-hook setup (Windows)

Some Codex builds require trusting the project before repo-local hooks load. For local demos, point `%USERPROFILE%\.codex\hooks.json` at `codex_user_hook.ps1`. The wrapper exits quietly outside this repository.

## Related docs

- VET sidecar CLI: [`tools/vet/README.md`](../vet/README.md)
- Architecture: [`docs/architecture-overview.md`](../../docs/architecture-overview.md)
