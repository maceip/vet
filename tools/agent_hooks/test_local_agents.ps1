param(
  [switch]$SkipClaude,
  [switch]$SkipCodex,
  [switch]$NoSeed
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
Set-Location $RepoRoot

if (-not $NoSeed) {
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/agent_hooks/invoke_dpm_gate.ps1 -Agent claude -Reset | Out-Null
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/agent_hooks/invoke_dpm_gate.ps1 -Agent claude -DemoSeed | Out-Null
}

Write-Host "DPM gate status:"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/agent_hooks/invoke_dpm_gate.ps1 -Agent claude -Status

if (-not $SkipClaude) {
  Write-Host "`nClaude hook smoke:"
  claude -p "Reply with exactly DPM_HOOK_READY. Do not use tools." `
    --model haiku `
    --output-format stream-json `
    --include-hook-events `
    --verbose `
    --max-budget-usd 0.15
}

if (-not $SkipCodex) {
  Write-Host "`nCodex hook smoke:"
  Write-Host "Note: Codex exec currently proves the local binary boots; interactive Codex uses the installed user hook layer."
  "" | codex exec -C $RepoRoot --json --enable hooks `
    "Reply with exactly DPM_HOOK_READY. Do not use tools."
}
