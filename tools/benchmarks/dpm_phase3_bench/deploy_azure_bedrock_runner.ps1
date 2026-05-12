[CmdletBinding()]
param(
  [string]$ResourceGroup = "dpm-phase3-bench-rg",
  [string]$Location = "westeurope",
  [string]$ContainerName = "dpm-phase3-bedrock-bench",
  [string]$Image = "python:3.12-slim",
  [string]$RepoUrl = "https://github.com/maceip/TiHKAL.git",
  [string]$Branch = "phase3-bench",
  [string]$AwsCredentialsFile = "Z:\home\pooppoop\.aws\credentials",
  [string]$AwsProfile = "default",
  [string]$BedrockRegion = "eu-north-1",
  [string]$BedrockModelId = "eu.anthropic.claude-opus-4-5-20251101-v1:0",
  [ValidateSet("smoke", "full")]
  [string]$Matrix = "smoke",
  [int]$BudgetChars = 1338,
  [int]$MaxTokensCap = 1024,
  [switch]$SkipResourceGroupCreate
)

$ErrorActionPreference = "Stop"

function Require-Command([string]$Name) {
  if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
    throw "Required command '$Name' was not found on PATH."
  }
}

function Get-AwsConfigValue([string]$Key) {
  $oldCreds = $env:AWS_SHARED_CREDENTIALS_FILE
  try {
    $env:AWS_SHARED_CREDENTIALS_FILE = $AwsCredentialsFile
    $value = & aws configure get $Key --profile $AwsProfile
    if ($LASTEXITCODE -ne 0) {
      throw "aws configure get $Key failed for profile '$AwsProfile'."
    }
    return ($value | Out-String).Trim()
  } finally {
    $env:AWS_SHARED_CREDENTIALS_FILE = $oldCreds
  }
}

Require-Command az
Require-Command aws

if (-not (Test-Path -LiteralPath $AwsCredentialsFile)) {
  throw "AWS credentials file not found: $AwsCredentialsFile"
}

$null = & az account show --output none
if ($LASTEXITCODE -ne 0) {
  throw "Azure CLI is not logged in. Run 'az login' first."
}

$accessKey = Get-AwsConfigValue "aws_access_key_id"
$secretKey = Get-AwsConfigValue "aws_secret_access_key"
$sessionToken = Get-AwsConfigValue "aws_session_token"
if ([string]::IsNullOrWhiteSpace($accessKey) -or
    [string]::IsNullOrWhiteSpace($secretKey)) {
  throw "AWS profile '$AwsProfile' is missing aws_access_key_id or aws_secret_access_key."
}

if (-not $SkipResourceGroupCreate) {
  & az group create `
    --name $ResourceGroup `
    --location $Location `
    --output none | Out-Null
}

if ($Matrix -eq "smoke") {
  $benchArgs = @(
    "--conditions raw_oracle",
    "--limit_cases 1",
    "--budget_chars $BudgetChars",
    "--run_id azure-bedrock-smoke",
    "--model_id bedrock:$BedrockModelId",
    "--output /work/phase3-bedrock-smoke.jsonl"
  ) -join " "
} else {
  $benchArgs = @(
    "--conditions raw_oracle,rolling_summary,dpm_phase3_checkpoint",
    "--budget_chars $BudgetChars",
    "--repeat 1",
    "--run_id azure-bedrock-full",
    "--model_id bedrock:$BedrockModelId",
    "--output /work/phase3-bedrock-full.jsonl"
  ) -join " "
}

$runner = @"
set -euo pipefail
apt-get update >/dev/null
apt-get install -y --no-install-recommends git ca-certificates >/dev/null
rm -rf /var/lib/apt/lists/*
python -m pip install --no-cache-dir boto3 >/dev/null
git clone --depth 1 --branch '$Branch' '$RepoUrl' /work/repo
cd /work/repo
python -m py_compile tools/benchmarks/dpm_phase3_bench/bedrock_adapter.py tools/benchmarks/dpm_phase3_bench/memory_agents.py
BENCH_USE_BEDROCK=1 BEDROCK_AWS_REGION='$BedrockRegion' BEDROCK_MODEL_ID='$BedrockModelId' BEDROCK_MAX_TOKENS_CAP='$MaxTokensCap' python tools/benchmarks/dpm_phase3_bench/run_phase3_bench.py --fixtures tools/benchmarks/dpm_phase3_bench/fixtures/real_sessions $benchArgs
cat /work/phase3-bedrock-*.jsonl
"@

$commandLine = "/bin/bash -lc " + [System.Management.Automation.Language.CodeGeneration]::QuoteArgument($runner)

$secureEnv = @(
  "AWS_ACCESS_KEY_ID=$accessKey",
  "AWS_SECRET_ACCESS_KEY=$secretKey"
)
if (-not [string]::IsNullOrWhiteSpace($sessionToken)) {
  $secureEnv += "AWS_SESSION_TOKEN=$sessionToken"
}

& az container create `
  --resource-group $ResourceGroup `
  --name $ContainerName `
  --location $Location `
  --image $Image `
  --os-type Linux `
  --restart-policy Never `
  --cpu 1 `
  --memory 2 `
  --command-line $commandLine `
  --environment-variables `
    AWS_DEFAULT_REGION=$BedrockRegion `
    AWS_REGION=$BedrockRegion `
    BEDROCK_AWS_REGION=$BedrockRegion `
    BEDROCK_MODEL_ID=$BedrockModelId `
    BEDROCK_MAX_TOKENS_CAP=$MaxTokensCap `
    BENCH_USE_BEDROCK=1 `
  --secure-environment-variables $secureEnv `
  --output table

Write-Host ""
Write-Host "Container submitted. Follow logs with:"
Write-Host "az container logs --resource-group $ResourceGroup --name $ContainerName --follow"
