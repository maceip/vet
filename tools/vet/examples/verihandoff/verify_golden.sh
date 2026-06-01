#!/usr/bin/env bash
# CI/local gate: committed golden session must verify unchanged.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
VET="${VET_BIN:-${REPO_ROOT}/bazel-bin/tools/vet/vet}"
FIXTURE_DIR="${SCRIPT_DIR}/fixtures/golden"
ROOT="${FIXTURE_DIR}/.vet"
TENANT="local"
SESSION="verihandoff-golden"
BUNDLE="${FIXTURE_DIR}/handoff.json"
EXPECTED="${FIXTURE_DIR}/verify-pass.json"

if [[ ! -x "${VET}" ]]; then
  echo "vet binary not found at ${VET}" >&2
  exit 1
fi
if [[ ! -f "${BUNDLE}" || ! -f "${EXPECTED}" ]]; then
  echo "golden fixtures missing under ${FIXTURE_DIR}" >&2
  exit 1
fi

ACTUAL="$("${VET}" verify --root "${ROOT}" --tenant "${TENANT}" --session "${SESSION}" \
  --bundle "${BUNDLE}" --json || true)"

python3 - <<'PY' "${ACTUAL}" "${EXPECTED}"
import json
import sys

actual = json.loads(sys.argv[1])
expected = json.loads(open(sys.argv[2], encoding="utf-8").read())

if not actual.get("verified"):
    print("golden verify failed: verified is not true", file=sys.stderr)
    print(json.dumps(actual, indent=2), file=sys.stderr)
    sys.exit(1)

if actual.get("checks_failed"):
    print("golden verify failed: unexpected checks_failed", file=sys.stderr)
    print(json.dumps(actual, indent=2), file=sys.stderr)
    sys.exit(1)

for check in expected.get("checks_passed", []):
    if check not in actual.get("checks_passed", []):
        print(f"golden verify failed: missing check {check}", file=sys.stderr)
        sys.exit(1)

print("golden verify passed")
PY
