#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
APP_HOST="${APP_HOST:-127.0.0.1}"
APP_PORT="${APP_PORT:-$("${ROOT_DIR}/scripts/find_free_port.sh" 18080 "${APP_HOST}")}"

if [[ "${BUILD_DIR}" != /* ]]; then
  BUILD_DIR="${ROOT_DIR}/${BUILD_DIR}"
fi

"${ROOT_DIR}/scripts/build.sh"

ctest --test-dir "${BUILD_DIR}" --output-on-failure

BINARY="${BUILD_DIR}/vix-arena"
if [[ ! -x "${BINARY}" ]]; then
  echo "missing binary: ${BINARY}" >&2
  exit 1
fi

LOG_FILE="$(mktemp)"
DATA_DIR="$(mktemp -d)"
PID=""
cleanup() {
  if [[ -n "${PID}" ]] && kill -0 "${PID}" >/dev/null 2>&1; then
    kill "${PID}" >/dev/null 2>&1 || true
    wait "${PID}" >/dev/null 2>&1 || true
  fi
  rm -f "${LOG_FILE}"
  rm -rf "${DATA_DIR}"
}
trap cleanup EXIT

(
  cd "${ROOT_DIR}"
  APP_HOST="${APP_HOST}" APP_PORT="${APP_PORT}" DATA_DIR="${DATA_DIR}" "${BINARY}"
) >"${LOG_FILE}" 2>&1 &
PID="$!"

for _ in {1..60}; do
  if curl -fsS "http://${APP_HOST}:${APP_PORT}/health" >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "${PID}" >/dev/null 2>&1; then
    echo "server exited during startup" >&2
    sed -n '1,120p' "${LOG_FILE}" >&2
    exit 1
  fi
  sleep 0.1
done

curl -fsS "http://${APP_HOST}:${APP_PORT}/health" | grep -q '"status":"ok"'
curl -fsS "http://${APP_HOST}:${APP_PORT}/ready" | grep -q '"ready":true'
curl -fsS "http://${APP_HOST}:${APP_PORT}/api/state" | grep -q '"service":"vix-arena"'
curl -fsS "http://${APP_HOST}:${APP_PORT}/api/stats" | grep -q '"tickRateTarget"'
curl -fsS "http://${APP_HOST}:${APP_PORT}/api/leaderboard" | grep -q '"entries"'
curl -fsS "http://${APP_HOST}:${APP_PORT}/api/matches" | grep -q '"matches"'
curl -fsS "http://${APP_HOST}:${APP_PORT}/metrics" | grep -q "vix_arena_up 1"
curl -fsS "http://${APP_HOST}:${APP_PORT}/metrics" | grep -q "vix_arena_leaderboard_entries"
curl -fsS "http://${APP_HOST}:${APP_PORT}/metrics" | grep -q "vix_arena_postgres_enabled"
curl -fsSI "http://${APP_HOST}:${APP_PORT}/" | grep -q "200 OK"
curl -fsSI "http://${APP_HOST}:${APP_PORT}/" | grep -qi "x-content-type-options: nosniff"
curl -fsSI "http://${APP_HOST}:${APP_PORT}/" | grep -qi "content-security-policy:"
curl -fsSI "http://${APP_HOST}:${APP_PORT}/" | grep -qi "x-frame-options: DENY"
curl -fsSI "http://${APP_HOST}:${APP_PORT}/docs" | grep -q "200 OK"
curl -fsSI "http://${APP_HOST}:${APP_PORT}/stats" | grep -q "200 OK"
bad_origin_status="$(
  curl -sS -o /dev/null -w "%{http_code}" \
    --max-time 2 \
    -H "Connection: Upgrade" \
    -H "Upgrade: websocket" \
    -H "Sec-WebSocket-Version: 13" \
    -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" \
    -H "Origin: https://evil.example" \
    "http://${APP_HOST}:${APP_PORT}/ws"
)"
if [[ "${bad_origin_status}" != "403" ]]; then
  echo "expected bad websocket origin to return 403, got ${bad_origin_status}" >&2
  exit 1
fi

missing_origin_status="$(
  curl -sS -o /dev/null -w "%{http_code}" \
    --max-time 2 \
    -H "Connection: Upgrade" \
    -H "Upgrade: websocket" \
    -H "Sec-WebSocket-Version: 13" \
    -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" \
    "http://${APP_HOST}:${APP_PORT}/ws"
)"
if [[ "${ALLOW_MISSING_ORIGIN:-false}" == "false" && "${missing_origin_status}" != "403" ]]; then
  echo "expected missing websocket origin to return 403, got ${missing_origin_status}" >&2
  exit 1
fi

good_origin_status="$(
  curl -sS -o /dev/null -w "%{http_code}" \
    --max-time 2 \
    -H "Connection: Upgrade" \
    -H "Upgrade: websocket" \
    -H "Sec-WebSocket-Version: 13" \
    -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" \
    -H "Origin: http://${APP_HOST}:${APP_PORT}" \
    "http://${APP_HOST}:${APP_PORT}/ws" 2>/dev/null || true
)"
if [[ "${good_origin_status}" != "101" ]]; then
  echo "expected allowed websocket origin to return 101, got ${good_origin_status}" >&2
  exit 1
fi

echo "check ok: build + tests + local HTTP/metrics/security smoke tests passed on ${APP_HOST}:${APP_PORT}"
