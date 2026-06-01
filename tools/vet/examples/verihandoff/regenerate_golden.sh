#!/usr/bin/env bash
# Regenerate committed golden fixtures after intentional verify/schema changes.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
VET="${VET_BIN:-${REPO_ROOT}/bazel-bin/tools/vet/vet}"
FIXTURE_DIR="${SCRIPT_DIR}/fixtures/golden"

if [[ ! -x "${VET}" ]]; then
  echo "vet binary not found at ${VET}" >&2
  exit 1
fi

rm -rf "${FIXTURE_DIR}"
mkdir -p "${FIXTURE_DIR}"

"${VET}" init --root "${FIXTURE_DIR}/.vet" --tenant local --session verihandoff-golden
"${VET}" record --root "${FIXTURE_DIR}/.vet" --tenant local --session verihandoff-golden \
  --type user \
  --payload "We ship correction-aware handoffs before the benchmark release."
"${VET}" correction --root "${FIXTURE_DIR}/.vet" --tenant local --session verihandoff-golden \
  --text "Stale escape metric definition changed." \
  --invalidated-fact "stale_memory_escape counts any stale text in memory" \
  --replacement-fact "stale escape counts only when stale facts reach the final answer"
"${VET}" record --root "${FIXTURE_DIR}/.vet" --tenant local --session verihandoff-golden \
  --type model \
  --payload "Handoff bundles must verify against the live append-only log."
"${VET}" handoff --root "${FIXTURE_DIR}/.vet" --tenant local --session verihandoff-golden \
  --task "Continue the benchmark release notes" \
  --format json \
  --out "${FIXTURE_DIR}/handoff.json"
"${VET}" verify --root "${FIXTURE_DIR}/.vet" --tenant local --session verihandoff-golden \
  --bundle "${FIXTURE_DIR}/handoff.json" --json > "${FIXTURE_DIR}/verify-pass.json"

echo "regenerated ${FIXTURE_DIR}"
