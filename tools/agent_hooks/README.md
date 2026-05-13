# DPM Agent Hooks

This folder contains a shared gate-mode hook adapter for local CLI coding
agents. It is intentionally a thin demo layer over the Phase 3 DPM substrate:
normal rolling memory remains useful, while the hook blocks or patches action
boundaries when active correction directives would make the pending action stale.

Runtime state is written to `.dpm-agent-hooks/` and ignored by git.

## Supported adapter configs

- Claude Code: `.claude/settings.json`
- Codex CLI: `.codex/config.toml`
- Gemini CLI: `.gemini/settings.json`
- Cursor Agent CLI: `.cursor/hooks.json`
- GitHub Copilot CLI / cloud agent: `.github/hooks/dpm-gate.json`

## Smoke commands

```powershell
python tools/agent_hooks/dpm_gate.py --agent claude --reset
python tools/agent_hooks/dpm_gate.py --agent claude --demo-seed
python tools/agent_hooks/dpm_gate.py --agent claude --status
```

Synthetic denial example:

```powershell
'{"hook_event_name":"PreToolUse","tool_name":"Write","tool_input":{"content":"transport as the main result"}}' |
  python tools/agent_hooks/dpm_gate.py --agent claude
```

Expected result: a structured deny response pointing at
`.dpm-agent-hooks/ACTIVE_CONTEXT.md`.

## Cursor fallback wrapper

Cursor CLI exposes hooks, but support varies by surface and version. The checked
in `.cursor/hooks.json` uses native hooks. For a headless smoke where hooks are
not firing, use:

```powershell
python tools/agent_hooks/cursor_agent_gate.py --prompt "make a harmless change"
```

That wrapper runs the prompt through `cursor-agent -p --output-format stream-json`
and records the stream in the same DPM demo ledger.

## Local Codex user-hook setup

Some Codex builds do not load repo-local hooks in non-interactive `exec` until
the project layer is trusted. For the local Windows demo, point
`%USERPROFILE%\.codex\hooks.json` at `codex_user_hook.ps1`. The wrapper exits
quietly outside this repository, so it will not gate other workspaces.
