[CmdletBinding()]
param(
  [string]$Output = ".smoke-bedrock-mantle-opus46.jsonl",
  [string]$ModelId = "anthropic.claude-opus-4-6-v1",
  [string]$BaseUrl = "https://bedrock-mantle.eu-north-1.api.aws/v1",
  [int]$BudgetChars = 1338,
  [int]$MaxTokensCap = 512
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($env:OPENAI_API_KEY)) {
  throw @"
Set OPENAI_API_KEY in this shell before running. For Bedrock Mantle, this is
the Bedrock API key value. Do not commit the key to the repo.

Example:
  `$env:OPENAI_API_KEY = '<bedrock api key>'
"@
}

$env:BENCH_USE_BEDROCK_MANTLE = "1"
$env:OPENAI_BASE_URL = $BaseUrl
$env:BEDROCK_MANTLE_MODEL_ID = $ModelId
$env:BEDROCK_MANTLE_MAX_TOKENS_CAP = [string]$MaxTokensCap

python tools\benchmarks\dpm_phase3_bench\run_phase3_bench.py `
  --fixtures tools\benchmarks\dpm_phase3_bench\fixtures\real_sessions `
  --conditions raw_oracle `
  --limit_cases 1 `
  --budget_chars $BudgetChars `
  --run_id bedrock-mantle-opus46-smoke `
  --model_id "bedrock-mantle:$ModelId" `
  --output $Output
