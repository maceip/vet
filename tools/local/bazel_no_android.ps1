param(
  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]] $BazelArgs
)

$ErrorActionPreference = "Stop"

Remove-Item Env:ANDROID_NDK_HOME -ErrorAction SilentlyContinue
Remove-Item Env:ANDROID_NDK_ROOT -ErrorAction SilentlyContinue
$env:BAZEL_SH = "C:\Program Files\Git\bin\bash.exe"

& bazelisk @BazelArgs --shell_executable="C:/Program Files/Git/bin/bash.exe"
exit $LASTEXITCODE
