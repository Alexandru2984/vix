#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_IP="${SOURCE_IP:-}"
ORIGIN_SSH="${ORIGIN_SSH:-}"
TARGET="${TARGET:-https://vix.micutu.com}"
KEEP_BENCHMARK_PROFILE="${KEEP_BENCHMARK_PROFILE:-false}"
RUN_BENCHMARK_GATE="${RUN_BENCHMARK_GATE:-false}"
BENCHMARK_SUITE="${BENCHMARK_SUITE:-standard}"
RESULT_DIR="${RESULT_DIR:-${ROOT_DIR}/benchmark-results/one-step-$(date -u +%Y%m%dT%H%M%SZ)}"

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
  RESULT_DIR=benchmark-results/run-name
  BENCHMARK_SUITE=standard      Use standard or extreme.
  RUN_BENCHMARK_GATE=true       Run scripts/benchmark_gate.mjs after the suite.
  BENCH_GATE_*                  Thresholds consumed by benchmark_gate.mjs.
EOF
  exit 2
fi

case "${BENCHMARK_SUITE}" in
  standard|extreme)
    ;;
  *)
    echo "invalid BENCHMARK_SUITE=${BENCHMARK_SUITE}; expected standard or extreme" >&2
    exit 2
    ;;
esac

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

echo "running ${BENCHMARK_SUITE} benchmark suite against ${TARGET}"
if [[ "${BENCHMARK_SUITE}" == "extreme" ]]; then
  TARGET="${TARGET}" RESULT_DIR="${RESULT_DIR}" "${ROOT_DIR}/scripts/benchmark_extreme.sh"
else
  TARGET="${TARGET}" RESULT_DIR="${RESULT_DIR}" "${ROOT_DIR}/scripts/benchmark_suite.sh"
fi

if [[ "${RUN_BENCHMARK_GATE}" == "true" ]]; then
  echo "running benchmark gate for ${RESULT_DIR}"
  node "${ROOT_DIR}/scripts/benchmark_gate.mjs" "${RESULT_DIR}"
fi
