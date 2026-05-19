#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${TARGET:-https://vix.micutu.com}"
ORIGIN_IP="${ORIGIN_IP:-}"
DURATION_SECONDS="${DURATION_SECONDS:-90}"
RESULT_DIR="${RESULT_DIR:-${ROOT_DIR}/benchmark-results/extreme-$(date -u +%Y%m%dT%H%M%SZ)}"

WRK_THREADS="${WRK_THREADS:-8}"
WRK_SPIKE_CONNECTIONS="${WRK_SPIKE_CONNECTIONS:-600}"
WRK_MIX_CONNECTIONS="${WRK_MIX_CONNECTIONS:-300}"
HEY_MIX_CONNECTIONS="${HEY_MIX_CONNECTIONS:-150}"

WS_DENSE_CLIENTS="${WS_DENSE_CLIENTS:-256}"
WS_DENSE_ROOMS="${WS_DENSE_ROOMS:-1}"
WS_SHARDED_CLIENTS="${WS_SHARDED_CLIENTS:-500}"
WS_SHARDED_ROOMS="${WS_SHARDED_ROOMS:-10}"
WS_MIXED_CLIENTS="${WS_MIXED_CLIENTS:-300}"
WS_MIXED_ROOMS="${WS_MIXED_ROOMS:-6}"
WS_INPUT_EVERY_MS="${WS_INPUT_EVERY_MS:-50}"
WS_CHAT_EVERY_MS="${WS_CHAT_EVERY_MS:-1200}"
WS_ABILITY_EVERY_MS="${WS_ABILITY_EVERY_MS:-1800}"
WS_RAMP_MS="${WS_RAMP_MS:-15000}"
WS_CHURN_EVERY_MS="${WS_CHURN_EVERY_MS:-5000}"
WS_CHURN_PERCENT="${WS_CHURN_PERCENT:-5}"

TARGET="${TARGET%/}"
mkdir -p "${RESULT_DIR}"

finalize() {
  local status=$?
  trap - EXIT

  curl -fsS "${TARGET}/api/stats" >"${RESULT_DIR}/stats-after.json" || true

  {
    echo "target=${TARGET}"
    echo "durationSeconds=${DURATION_SECONDS}"
    echo "resultDir=${RESULT_DIR}"
    echo "exitStatus=${status}"
    echo
    grep -R "Requests/sec\\|Transfer/sec\\|Latency\\|Status code distribution\\|\\[200\\]\\|\\[429\\]\\|welcomed\\|welcomedCount\\|connectAttempts\\|serverErrors\\|expectedDefensiveErrors\\|unexpectedServerErrors\\|snapshots\\|p95\\|load test ok\\|load test failed" "${RESULT_DIR}"/*.log 2>/dev/null || true
  } | tee "${RESULT_DIR}/summary.txt"

  if command -v node >/dev/null 2>&1; then
    node "${ROOT_DIR}/scripts/benchmark_report.mjs" "${RESULT_DIR}" || true
  fi

  if [[ "${status}" -eq 0 ]]; then
    echo "extreme benchmark done: ${RESULT_DIR}"
  else
    echo "extreme benchmark failed with status ${status}: ${RESULT_DIR}" >&2
  fi
  exit "${status}"
}

trap finalize EXIT

target_host="$(
  TARGET_FOR_PARSE="${TARGET}" python3 - <<'PY' 2>/dev/null || true
from urllib.parse import urlparse
import os
print(urlparse(os.environ["TARGET_FOR_PARSE"]).hostname or "")
PY
)"

if [[ -n "${ORIGIN_IP}" && -n "${target_host}" ]] && command -v getent >/dev/null 2>&1; then
  if ! getent ahostsv4 "${target_host}" | awk '{print $1}' | grep -qx "${ORIGIN_IP}"; then
    echo "warning: ${target_host} does not resolve to ORIGIN_IP=${ORIGIN_IP}; you may be benchmarking Cloudflare instead of origin" >&2
    echo "${ORIGIN_IP} ${target_host}" >&2
  fi
fi

run_log() {
  local name="$1"
  shift
  local log="${RESULT_DIR}/${name}.log"
  echo "==> ${name}: $*" | tee "${log}"
  "$@" 2>&1 | tee -a "${log}"
}

run_bg_log() {
  local name="$1"
  shift
  local log="${RESULT_DIR}/${name}.log"
  echo "==> ${name}: $*" | tee "${log}" >&2
  "$@" > >(tee -a "${log}" >&2) 2> >(tee -a "${log}" >&2) &
  echo "$!"
}

wait_all() {
  local failed=0
  local pid
  for pid in "$@"; do
    if ! wait "${pid}"; then
      failed=1
    fi
  done
  return "${failed}"
}

curl -fsS "${TARGET}/api/stats" >"${RESULT_DIR}/stats-before.json" || true

wrk_bin=""
if command -v wrk6 >/dev/null 2>&1; then
  wrk_bin="wrk6"
elif command -v wrk >/dev/null 2>&1; then
  wrk_bin="wrk"
fi

if [[ -z "${wrk_bin}" ]]; then
  echo "missing wrk/wrk6" >&2
  exit 2
fi

if ! command -v node >/dev/null 2>&1 || ! node -e 'require("ws")' >/dev/null 2>&1; then
  echo "missing node or npm package ws" >&2
  exit 2
fi

echo "phase 1/4: HTTP spike"
run_log "wrk-spike-home-c${WRK_SPIKE_CONNECTIONS}" "${wrk_bin}" -t"${WRK_THREADS}" -c"${WRK_SPIKE_CONNECTIONS}" -d"${DURATION_SECONDS}s" "${TARGET}/"
run_log "wrk-spike-state-c${WRK_SPIKE_CONNECTIONS}" "${wrk_bin}" -t"${WRK_THREADS}" -c"${WRK_SPIKE_CONNECTIONS}" -d"${DURATION_SECONDS}s" "${TARGET}/api/state"
run_log "wrk-spike-stats-c${WRK_SPIKE_CONNECTIONS}" "${wrk_bin}" -t"${WRK_THREADS}" -c"${WRK_SPIKE_CONNECTIONS}" -d"${DURATION_SECONDS}s" "${TARGET}/api/stats"

echo "phase 2/4: dense single-room WebSocket with chat and abilities"
run_log "ws-dense-c${WS_DENSE_CLIENTS}-r${WS_DENSE_ROOMS}" \
  env BASE_URL="${TARGET}" \
    VIX_LOAD_CLIENTS="${WS_DENSE_CLIENTS}" \
    VIX_LOAD_ROOMS="${WS_DENSE_ROOMS}" \
    VIX_LOAD_DURATION_MS="$((DURATION_SECONDS * 1000))" \
    VIX_LOAD_INPUT_EVERY_MS="${WS_INPUT_EVERY_MS}" \
    VIX_LOAD_CHAT_EVERY_MS="${WS_CHAT_EVERY_MS}" \
    VIX_LOAD_ABILITY_EVERY_MS="${WS_ABILITY_EVERY_MS}" \
    VIX_LOAD_RAMP_MS="${WS_RAMP_MS}" \
    VIX_LOAD_EXPECT_DEFENSIVE_ERRORS=true \
    node "${ROOT_DIR}/scripts/ws_load_test.mjs"

echo "phase 3/4: sharded WebSocket with churn"
run_log "ws-sharded-churn-c${WS_SHARDED_CLIENTS}-r${WS_SHARDED_ROOMS}" \
  env BASE_URL="${TARGET}" \
    VIX_LOAD_CLIENTS="${WS_SHARDED_CLIENTS}" \
    VIX_LOAD_ROOMS="${WS_SHARDED_ROOMS}" \
    VIX_LOAD_DURATION_MS="$((DURATION_SECONDS * 1000))" \
    VIX_LOAD_INPUT_EVERY_MS="${WS_INPUT_EVERY_MS}" \
    VIX_LOAD_CHAT_EVERY_MS="${WS_CHAT_EVERY_MS}" \
    VIX_LOAD_ABILITY_EVERY_MS="${WS_ABILITY_EVERY_MS}" \
    VIX_LOAD_RAMP_MS="${WS_RAMP_MS}" \
    VIX_LOAD_RECONNECT_EVERY_MS="${WS_CHURN_EVERY_MS}" \
    VIX_LOAD_RECONNECT_PERCENT="${WS_CHURN_PERCENT}" \
    VIX_LOAD_EXPECT_DEFENSIVE_ERRORS=true \
    node "${ROOT_DIR}/scripts/ws_load_test.mjs"

echo "phase 4/4: mixed HTTP and WebSocket contention"
pids=()
pids+=("$(run_bg_log "mixed-wrk-home-c${WRK_MIX_CONNECTIONS}" "${wrk_bin}" -t"${WRK_THREADS}" -c"${WRK_MIX_CONNECTIONS}" -d"${DURATION_SECONDS}s" "${TARGET}/")")
pids+=("$(run_bg_log "mixed-wrk-state-c${WRK_MIX_CONNECTIONS}" "${wrk_bin}" -t"${WRK_THREADS}" -c"${WRK_MIX_CONNECTIONS}" -d"${DURATION_SECONDS}s" "${TARGET}/api/state")")
if command -v hey >/dev/null 2>&1; then
  pids+=("$(run_bg_log "mixed-hey-stats-c${HEY_MIX_CONNECTIONS}" hey -z "${DURATION_SECONDS}s" -c "${HEY_MIX_CONNECTIONS}" "${TARGET}/api/stats")")
fi
pids+=("$(run_bg_log "mixed-ws-c${WS_MIXED_CLIENTS}-r${WS_MIXED_ROOMS}" \
  env BASE_URL="${TARGET}" \
    VIX_LOAD_CLIENTS="${WS_MIXED_CLIENTS}" \
    VIX_LOAD_ROOMS="${WS_MIXED_ROOMS}" \
    VIX_LOAD_DURATION_MS="$((DURATION_SECONDS * 1000))" \
    VIX_LOAD_INPUT_EVERY_MS="${WS_INPUT_EVERY_MS}" \
    VIX_LOAD_CHAT_EVERY_MS="${WS_CHAT_EVERY_MS}" \
    VIX_LOAD_ABILITY_EVERY_MS="${WS_ABILITY_EVERY_MS}" \
    VIX_LOAD_RAMP_MS="${WS_RAMP_MS}" \
    VIX_LOAD_EXPECT_DEFENSIVE_ERRORS=true \
    node "${ROOT_DIR}/scripts/ws_load_test.mjs")")

if ! wait_all "${pids[@]}"; then
  echo "one or more mixed benchmark workers failed" >&2
  exit 1
fi
