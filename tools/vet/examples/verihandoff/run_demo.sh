#!/usr/bin/env bash
# VeriHandoff: end-to-end verification demo for the VET sidecar.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
VET="${VET_BIN:-${REPO_ROOT}/bazel-bin/tools/vet/vet}"
DEMO_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/verihandoff.XXXXXX")"

cleanup() {
  rm -rf "${DEMO_ROOT}"
}
trap cleanup EXIT

if [[ ! -x "${VET}" ]]; then
  echo "vet binary not found at ${VET}" >&2
  echo "Build with: bazelisk build --config=vet_release_no_android //tools/vet:vet" >&2
  exit 1
fi

echo "== VeriHandoff demo =="
echo "demo root: ${DEMO_ROOT}"
echo

cd "${DEMO_ROOT}"

"${VET}" init --root .vet --tenant local --session verihandoff-demo

"${VET}" record --root .vet --tenant local --session verihandoff-demo \
  --type user \
  --payload "We ship correction-aware handoffs before the benchmark release."

"${VET}" correction --root .vet --tenant local --session verihandoff-demo \
  --text "Stale escape metric definition changed." \
  --invalidated-fact "stale_memory_escape counts any stale text in memory" \
  --replacement-fact "stale escape counts only when stale facts reach the final answer"

"${VET}" record --root .vet --tenant local --session verihandoff-demo \
  --type model \
  --payload "Handoff bundles must verify against the live append-only log."

"${VET}" handoff --root .vet --tenant local --session verihandoff-demo \
  --task "Continue the benchmark release notes" \
  --format json \
  --out handoff.json

echo
echo "== verify (expect VERIFIED) =="
VERIFY_JSON="$("${VET}" verify --root .vet --tenant local --session verihandoff-demo \
  --bundle handoff.json --json || true)"
echo "${VERIFY_JSON}"

if ! echo "${VERIFY_JSON}" | rg -q '"verified"[[:space:]]*:[[:space:]]*true'; then
  echo "expected verified=true" >&2
  exit 1
fi

echo
echo "== tamper log (expect FAILED) =="
echo '{"type":"internal","tenant_id":"local","session_id":"verihandoff-demo","payload":"host injected","timestamp_us":99}' \
  >> .vet/local/verihandoff-demo/events.dpmlog

TAMPER_JSON="$("${VET}" verify --root .vet --tenant local --session verihandoff-demo \
  --bundle handoff.json --json || true)"
echo "${TAMPER_JSON}"

if ! echo "${TAMPER_JSON}" | rg -q '"verified"[[:space:]]*:[[:space:]]*false'; then
  echo "expected verified=false after tamper" >&2
  exit 1
fi
if ! echo "${TAMPER_JSON}" | rg -q 'trace_digest_match'; then
  echo "expected trace_digest_match failure" >&2
  exit 1
fi

echo
echo "VeriHandoff demo passed."
