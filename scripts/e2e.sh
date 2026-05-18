#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
APP_HOST="${APP_HOST:-127.0.0.1}"
APP_PORT="${APP_PORT:-$("${ROOT_DIR}/scripts/find_free_port.sh" 18180 "${APP_HOST}")}"
PUBLIC_URL="${PUBLIC_URL:-http://${APP_HOST}:${APP_PORT}}"
ALLOWED_ORIGINS="${ALLOWED_ORIGINS:-http://${APP_HOST}:${APP_PORT}}"
ALLOW_MISSING_ORIGIN="${ALLOW_MISSING_ORIGIN:-false}"
DATABASE_URL="${DATABASE_URL:-}"

if [[ "${BUILD_DIR}" != /* ]]; then
  BUILD_DIR="${ROOT_DIR}/${BUILD_DIR}"
fi

if [[ "${SKIP_BUILD:-false}" != "true" ]]; then
  "${ROOT_DIR}/scripts/build.sh"
fi

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
  APP_HOST="${APP_HOST}" \
    APP_PORT="${APP_PORT}" \
    PUBLIC_URL="${PUBLIC_URL}" \
    ALLOWED_ORIGINS="${ALLOWED_ORIGINS}" \
    ALLOW_MISSING_ORIGIN="${ALLOW_MISSING_ORIGIN}" \
    DATABASE_URL="${DATABASE_URL}" \
    DATA_DIR="${DATA_DIR}" \
    "${BINARY}"
) >"${LOG_FILE}" 2>&1 &
PID="$!"

for _ in {1..80}; do
  if curl -fsS "http://${APP_HOST}:${APP_PORT}/health" >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "${PID}" >/dev/null 2>&1; then
    echo "server exited during e2e startup" >&2
    sed -n '1,160p' "${LOG_FILE}" >&2
    exit 1
  fi
  sleep 0.1
done

curl -fsS "http://${APP_HOST}:${APP_PORT}/health" >/dev/null
BASE_URL="http://${APP_HOST}:${APP_PORT}" npm run test:e2e

echo "e2e ok: browser checks passed on http://${APP_HOST}:${APP_PORT}"
