param(
  [ValidateSet("claude", "codex", "gemini", "cursor", "copilot")]
  [string]$Agent = "claude",
  [switch]$Reset,
  [switch]$DemoSeed,
  [switch]$Status
)

$ErrorActionPreference = "Stop"
$Script = Join-Path $PSScriptRoot "dpm_gate.py"
$Python = $null

$PyLauncher = Get-Command py.exe -ErrorAction SilentlyContinue
if ($PyLauncher) {
  $Python = @($PyLauncher.Source, "-3")
} else {
  $PythonExe = Get-Command python.exe -ErrorAction SilentlyContinue
  if ($PythonExe) {
    $Python = @($PythonExe.Source)
  }
}

if (-not $Python) {
  Write-Error "No Python runtime found for DPM gate hook"
  exit 127
}

$Args = @($Script, "--agent", $Agent)
if ($Reset) { $Args += "--reset" }
if ($DemoSeed) { $Args += "--demo-seed" }
if ($Status) { $Args += "--status" }

& $Python[0] @($Python[1..($Python.Length - 1)]) @Args
exit $LASTEXITCODE
