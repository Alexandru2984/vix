#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_IP="${SOURCE_IP:-}"
ORIGIN_SSH="${ORIGIN_SSH:-}"
TARGET="${TARGET:-https://vix.micutu.com}"
KEEP_BENCHMARK_PROFILE="${KEEP_BENCHMARK_PROFILE:-false}"

if [[ -z "${SOURCE_IP}" ]]; then
  SOURCE_IP="$(curl -fsS https://api.ipify.org 2>/dev/null || true)"
fi

if [[ -z "${SOURCE_IP}" || -z "${ORIGIN_SSH}" ]]; then
  cat >&2 <<'EOF'
Usage from the benchmark VPS:
  SOURCE_IP=81.181.166.237 ORIGIN_SSH=micu@ORIGIN_IP TARGET=https://vix.micutu.com scripts/benchmark_one_step.sh

Optional:
  KEEP_BENCHMARK_PROFILE=true   Keep benchmark profile enabled after tests.
  DURATION_SECONDS=30           Shorter benchmark duration.
EOF
  exit 2
fi

cleanup() {
  if [[ "${KEEP_BENCHMARK_PROFILE}" == "true" ]]; then
    echo "keeping benchmark profile enabled"
    return
  fi
  echo "disabling benchmark profile on origin"
  ssh "${ORIGIN_SSH}" "cd /home/micu/vix && scripts/benchmark_profile.sh disable" || true
}
trap cleanup EXIT

echo "enabling benchmark profile for ${SOURCE_IP} on ${ORIGIN_SSH}"
ssh "${ORIGIN_SSH}" "cd /home/micu/vix && scripts/benchmark_profile.sh enable '${SOURCE_IP}'"

echo "running benchmark suite against ${TARGET}"
TARGET="${TARGET}" "${ROOT_DIR}/scripts/benchmark_suite.sh"
