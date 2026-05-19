#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${TARGET:-https://vix.micutu.com}"
ORIGIN_IP="${ORIGIN_IP:-}"
DURATION_SECONDS="${DURATION_SECONDS:-60}"
RESULT_DIR="${RESULT_DIR:-${ROOT_DIR}/benchmark-results/$(date -u +%Y%m%dT%H%M%SZ)}"
HEY_CONCURRENCY_HOME="${HEY_CONCURRENCY_HOME:-50}"
HEY_CONCURRENCY_STATE="${HEY_CONCURRENCY_STATE:-100}"
WRK_THREADS="${WRK_THREADS:-4}"
WRK_CONNECTIONS="${WRK_CONNECTIONS:-100}"
WRK_HIGH_THREADS="${WRK_HIGH_THREADS:-8}"
WRK_HIGH_CONNECTIONS="${WRK_HIGH_CONNECTIONS:-200}"
WS_CLIENTS_LOW="${WS_CLIENTS_LOW:-50}"
WS_CLIENTS_HIGH="${WS_CLIENTS_HIGH:-100}"

TARGET="${TARGET%/}"
mkdir -p "${RESULT_DIR}"

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
    echo "add this on the benchmark VPS if you want direct-origin HTTPS:" >&2
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

curl -fsS "${TARGET}/api/stats" >"${RESULT_DIR}/stats-before.json" || true

if command -v hey >/dev/null 2>&1; then
  run_log "hey-home-c${HEY_CONCURRENCY_HOME}" hey -z "${DURATION_SECONDS}s" -c "${HEY_CONCURRENCY_HOME}" "${TARGET}/"
  run_log "hey-state-c${HEY_CONCURRENCY_STATE}" hey -z "${DURATION_SECONDS}s" -c "${HEY_CONCURRENCY_STATE}" "${TARGET}/api/state"
  run_log "hey-stats-c${HEY_CONCURRENCY_STATE}" hey -z "${DURATION_SECONDS}s" -c "${HEY_CONCURRENCY_STATE}" "${TARGET}/api/stats"
else
  echo "skip hey: command not found" | tee "${RESULT_DIR}/hey-skipped.log"
fi

wrk_bin=""
if command -v wrk6 >/dev/null 2>&1; then
  wrk_bin="wrk6"
elif command -v wrk >/dev/null 2>&1; then
  wrk_bin="wrk"
fi

if [[ -n "${wrk_bin}" ]]; then
  run_log "wrk-home-c${WRK_CONNECTIONS}" "${wrk_bin}" -t"${WRK_THREADS}" -c"${WRK_CONNECTIONS}" -d"${DURATION_SECONDS}s" "${TARGET}/"
  run_log "wrk-state-c${WRK_CONNECTIONS}" "${wrk_bin}" -t"${WRK_THREADS}" -c"${WRK_CONNECTIONS}" -d"${DURATION_SECONDS}s" "${TARGET}/api/state"
  run_log "wrk-home-c${WRK_HIGH_CONNECTIONS}" "${wrk_bin}" -t"${WRK_HIGH_THREADS}" -c"${WRK_HIGH_CONNECTIONS}" -d"${DURATION_SECONDS}s" "${TARGET}/"
else
  echo "skip wrk/wrk6: command not found" | tee "${RESULT_DIR}/wrk-skipped.log"
fi

if command -v node >/dev/null 2>&1 && node -e 'require("ws")' >/dev/null 2>&1; then
  run_log "ws-c${WS_CLIENTS_LOW}" env BASE_URL="${TARGET}" VIX_LOAD_CLIENTS="${WS_CLIENTS_LOW}" VIX_LOAD_DURATION_MS="$((DURATION_SECONDS * 1000))" node "${ROOT_DIR}/scripts/ws_load_test.mjs"
  run_log "ws-c${WS_CLIENTS_HIGH}" env BASE_URL="${TARGET}" VIX_LOAD_CLIENTS="${WS_CLIENTS_HIGH}" VIX_LOAD_DURATION_MS="$((DURATION_SECONDS * 1000))" node "${ROOT_DIR}/scripts/ws_load_test.mjs"
else
  echo "skip websocket load: node or npm package ws not available" | tee "${RESULT_DIR}/ws-skipped.log"
fi

curl -fsS "${TARGET}/api/stats" >"${RESULT_DIR}/stats-after.json" || true

{
  echo "target=${TARGET}"
  echo "durationSeconds=${DURATION_SECONDS}"
  echo "resultDir=${RESULT_DIR}"
  echo
  grep -R "Requests/sec\\|Status code distribution\\|\\[200\\]\\|\\[429\\]\\|Latency\\|welcomed\\|snapshots\\|p95\\|load test ok" "${RESULT_DIR}"/*.log 2>/dev/null || true
} | tee "${RESULT_DIR}/summary.txt"

echo "benchmark suite done: ${RESULT_DIR}"
