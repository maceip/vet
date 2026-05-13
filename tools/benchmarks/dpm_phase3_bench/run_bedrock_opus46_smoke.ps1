[CmdletBinding()]
param(
  [string]$Output = ".smoke-bedrock-opus46.jsonl",
  [string]$ModelId = "us.anthropic.claude-opus-4-6-v1",
  [string]$Region = "us-east-1",
  [int]$BudgetChars = 1338,
  [int]$MaxTokensCap = 512
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($env:AWS_BEARER_TOKEN_BEDROCK) -and
    [string]::IsNullOrWhiteSpace($env:AWS_SHARED_CREDENTIALS_FILE)) {
  throw @"
Set AWS_BEARER_TOKEN_BEDROCK in this shell before running, or configure normal
AWS credentials. Do not commit the key to the repo.

Example:
  `$env:AWS_BEARER_TOKEN_BEDROCK = '<bedrock api key>'
"@
}

$env:BENCH_USE_BEDROCK = "1"
$env:BEDROCK_AWS_REGION = $Region
$env:BEDROCK_MODEL_ID = $ModelId
$env:BEDROCK_MAX_TOKENS_CAP = [string]$MaxTokensCap

python tools\benchmarks\dpm_phase3_bench\run_phase3_bench.py `
  --fixtures tools\benchmarks\dpm_phase3_bench\fixtures\real_sessions `
  --conditions raw_oracle `
  --limit_cases 1 `
  --budget_chars $BudgetChars `
  --run_id bedrock-opus46-smoke `
  --model_id "bedrock:$ModelId" `
  --output $Output
