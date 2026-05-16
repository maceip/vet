#!/usr/bin/env bash
# Copyright 2026 Google LLC.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  tools/vet/install_agent_asset.sh codex [--scope project|user] [--repo PATH]
  tools/vet/install_agent_asset.sh claude [--scope project|user] [--repo PATH]
  tools/vet/install_agent_asset.sh gemini

Defaults:
  --scope project
  --repo current directory
EOF
}

agent="${1:-}"
if [[ -z "${agent}" || "${agent}" == "--help" || "${agent}" == "-h" ]]; then
  usage
  exit 0
fi
shift

scope="project"
repo="$(pwd)"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scope)
      scope="${2:?missing value for --scope}"
      shift 2
      ;;
    --scope=*)
      scope="${1#--scope=}"
      shift
      ;;
    --repo)
      repo="${2:?missing value for --repo}"
      shift 2
      ;;
    --repo=*)
      repo="${1#--repo=}"
      shift
      ;;
    *)
      echo "unknown flag: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

copy_asset() {
  local src="$1"
  local dest="$2"
  rm -rf "${dest}"
  mkdir -p "$(dirname "${dest}")"
  cp -R "${src}" "${dest}"
  echo "installed ${agent} VET asset: ${dest}"
}

case "${agent}" in
  codex)
    src="${script_dir}/agent_assets/codex/vet-sidecar"
    case "${scope}" in
      project) dest="${repo}/.agents/skills/vet-sidecar" ;;
      user) dest="${HOME}/.agents/skills/vet-sidecar" ;;
      *) echo "--scope must be project or user" >&2; exit 1 ;;
    esac
    copy_asset "${src}" "${dest}"
    ;;
  claude)
    src="${script_dir}/agent_assets/claude/vet-sidecar"
    case "${scope}" in
      project) dest="${repo}/.claude/skills/vet-sidecar" ;;
      user) dest="${HOME}/.claude/skills/vet-sidecar" ;;
      *) echo "--scope must be project or user" >&2; exit 1 ;;
    esac
    copy_asset "${src}" "${dest}"
    ;;
  gemini)
    src="${script_dir}/agent_assets/gemini/vet-sidecar"
    dest="${HOME}/.gemini/extensions/vet-sidecar"
    copy_asset "${src}" "${dest}"
    ;;
  *)
    echo "unknown agent: ${agent}" >&2
    usage >&2
    exit 1
    ;;
esac
