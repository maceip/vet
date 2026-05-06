#!/usr/bin/env bash
# Deploys the DPM cliff handoff broker as an AWS Lambda Function URL.
# Single endpoint, no API Gateway, no Cognito. Three routes inside.
#
# Prereqs in env:
#   AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, AWS_REGION
#   ROOT_PRIVATE_KEY_HEX, ROOT_PUBLIC_KEY_HEX  (Biscuit ed25519 pair)
#
# Usage:
#   ./deploy.sh
#
# Idempotent: re-running updates the function code + env without
# tearing down. Emits the Function URL on stdout when done.

set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

FN_NAME=${FN_NAME:-dpm-cliff-handoff-broker}
REGION=${AWS_REGION:-eu-north-1}
ROLE_NAME=${ROLE_NAME:-dpm-cliff-handoff-broker-role}
AUDIT_BUCKET=${AUDIT_BUCKET:-dpm-cliff-bench--eun1-az1--x-s3}
RUNTIME=${RUNTIME:-nodejs22.x}

# Stage tmp artifacts under the broker dir so both the bash heredoc and
# the windows aws.exe agree on the path.
STAGE_DIR="$HERE/.deploy"
mkdir -p "$STAGE_DIR"
ZIP_PATH="$STAGE_DIR/dpm-broker.zip"
POLICY_PATH="$STAGE_DIR/dpm-broker-policy.json"
to_aws_path() {
  # On Windows-bash convert to a windows-style path so aws.exe can read it.
  if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else echo "$1"; fi
}

if [[ -z "${ROOT_PRIVATE_KEY_HEX:-}" || -z "${ROOT_PUBLIC_KEY_HEX:-}" ]]; then
  echo "ROOT_PRIVATE_KEY_HEX and ROOT_PUBLIC_KEY_HEX must be set." >&2
  echo "Generate a pair with: node ./gen_keypair.mjs > .broker.env && source .broker.env" >&2
  exit 1
fi

echo "[deploy] installing deps for runtime"
npm install --silent --omit=dev --no-audit --no-fund

echo "[deploy] zipping function bundle"
rm -f "$ZIP_PATH"
if command -v zip >/dev/null 2>&1; then
  ( cd "$HERE" && zip -qr "$ZIP_PATH" handler.mjs package.json node_modules )
elif command -v powershell.exe >/dev/null 2>&1; then
  WIN_HERE=$(to_aws_path "$HERE")
  WIN_OUT=$(to_aws_path "$ZIP_PATH")
  powershell.exe -NoProfile -Command "
    Set-Location -LiteralPath '$WIN_HERE';
    Compress-Archive -Force -Path handler.mjs,package.json,node_modules -DestinationPath '$WIN_OUT'" >/dev/null
else
  echo "[deploy] need zip or powershell to bundle the function" >&2
  exit 1
fi
echo "[deploy] zip size: $(stat -c%s "$ZIP_PATH" 2>/dev/null || stat -f%z "$ZIP_PATH") bytes"

# 1. Ensure the IAM role exists with logs + S3 access on the audit bucket.
ASSUME_DOC='{
  "Version":"2012-10-17",
  "Statement":[{"Effect":"Allow","Principal":{"Service":"lambda.amazonaws.com"},"Action":"sts:AssumeRole"}]
}'
ROLE_ARN=$(aws iam get-role --role-name "$ROLE_NAME" --query 'Role.Arn' --output text 2>/dev/null || true)
if [[ -z "$ROLE_ARN" || "$ROLE_ARN" == "None" ]]; then
  echo "[deploy] creating IAM role $ROLE_NAME"
  ROLE_ARN=$(aws iam create-role --role-name "$ROLE_NAME" \
    --assume-role-policy-document "$ASSUME_DOC" \
    --query 'Role.Arn' --output text)
  aws iam attach-role-policy --role-name "$ROLE_NAME" \
    --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole >/dev/null
  ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)
  cat > "$POLICY_PATH" <<EOF
{
  "Version":"2012-10-17",
  "Statement":[
    {"Sid":"S3ExpressCreateSession",
     "Effect":"Allow",
     "Action":"s3express:CreateSession",
     "Resource":"arn:aws:s3express:${REGION}:${ACCOUNT_ID}:bucket/${AUDIT_BUCKET}"},
    {"Sid":"S3ExpressObjectAccess",
     "Effect":"Allow",
     "Action":["s3:GetObject","s3:PutObject","s3:DeleteObject","s3:HeadObject"],
     "Resource":"arn:aws:s3express:${REGION}:${ACCOUNT_ID}:bucket/${AUDIT_BUCKET}/*"}
  ]
}
EOF
  aws iam put-role-policy --role-name "$ROLE_NAME" \
    --policy-name dpm-broker-s3 \
    --policy-document "file://$(to_aws_path "$POLICY_PATH")" >/dev/null
  echo "[deploy] role $ROLE_ARN created — sleeping 10s for propagation"
  sleep 10
fi

# 2. Create-or-update the function.
ENV_VARS="Variables={ROOT_PRIVATE_KEY_HEX=$ROOT_PRIVATE_KEY_HEX,ROOT_PUBLIC_KEY_HEX=$ROOT_PUBLIC_KEY_HEX,AUDIT_BUCKET=$AUDIT_BUCKET,AUDIT_REGION=$REGION}"
if aws lambda get-function --function-name "$FN_NAME" --region "$REGION" >/dev/null 2>&1; then
  echo "[deploy] updating function code"
  aws lambda update-function-code --region "$REGION" \
    --function-name "$FN_NAME" \
    --zip-file "fileb://$(to_aws_path "$ZIP_PATH")" \
    --query '[FunctionArn,LastModified]' --output text
  echo "[deploy] waiting for code update to settle"
  aws lambda wait function-updated-v2 --region "$REGION" --function-name "$FN_NAME"
  aws lambda update-function-configuration --region "$REGION" \
    --function-name "$FN_NAME" \
    --runtime "$RUNTIME" \
    --handler handler.handler \
    --role "$ROLE_ARN" \
    --environment "$ENV_VARS" \
    --timeout 15 --memory-size 512 \
    --query 'LastModified' --output text >/dev/null
else
  echo "[deploy] creating function"
  aws lambda create-function --region "$REGION" \
    --function-name "$FN_NAME" \
    --runtime "$RUNTIME" \
    --handler handler.handler \
    --role "$ROLE_ARN" \
    --zip-file "fileb://$(to_aws_path "$ZIP_PATH")" \
    --environment "$ENV_VARS" \
    --timeout 15 --memory-size 512 \
    --query 'FunctionArn' --output text >/dev/null
fi

# 3. Create-or-update Function URL with permissive CORS for the gh-pages origin.
FN_URL_CFG=$(aws lambda get-function-url-config --region "$REGION" \
  --function-name "$FN_NAME" --query 'FunctionUrl' --output text 2>/dev/null || true)
if [[ -z "$FN_URL_CFG" || "$FN_URL_CFG" == "None" ]]; then
  echo "[deploy] creating function URL"
  FN_URL_CFG=$(aws lambda create-function-url-config --region "$REGION" \
    --function-name "$FN_NAME" \
    --auth-type NONE \
    --cors '{"AllowOrigins":["https://maceip.github.io","http://localhost:8765"],"AllowMethods":["POST"],"AllowHeaders":["authorization","content-type"]}' \
    --query 'FunctionUrl' --output text)
  aws lambda add-permission --region "$REGION" \
    --function-name "$FN_NAME" \
    --statement-id FunctionURLAllowPublic \
    --action lambda:InvokeFunctionUrl \
    --principal '*' \
    --function-url-auth-type NONE >/dev/null || true
fi

echo
echo "Function URL: $FN_URL_CFG"
echo "Routes:"
echo "  POST $FN_URL_CFG""delegate"
echo "  POST $FN_URL_CFG""presign"
echo "  POST $FN_URL_CFG""revoke"
