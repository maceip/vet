param(
  [string]$Agent = "codex"
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$Cwd = (Get-Location).Path

if (-not $Cwd.StartsWith($RepoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
  exit 0
}

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  (Join-Path $PSScriptRoot "invoke_dpm_gate.ps1") -Agent $Agent
exit $LASTEXITCODE
