#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="${VIX_DEPLOY_DIR:-/home/micu/vix}"
BRANCH="${VIX_DEPLOY_BRANCH:-main}"
SERVICE="${VIX_SERVICE:-vix-arena.service}"
RUN_CI="${VIX_RUN_CI_ON_DEPLOY:-true}"
LOCK_FILE="${VIX_DEPLOY_LOCK:-/tmp/vix-arena-deploy.lock}"

cd "${ROOT_DIR}"

if [[ ! "${BRANCH}" =~ ^[A-Za-z0-9._/-]+$ || "${BRANCH}" == *".."* || "${BRANCH}" == /* ]]; then
  echo "invalid deploy branch: ${BRANCH}" >&2
  exit 1
fi

exec 9>"${LOCK_FILE}"
if ! flock -n 9; then
  echo "deploy already running; lock: ${LOCK_FILE}" >&2
  exit 1
fi

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "missing required command: $1" >&2
    exit 1
  fi
}

require_cmd git
require_cmd cmake
require_cmd curl
require_cmd sudo
require_cmd systemctl
require_cmd nginx
require_cmd ss

if ! sudo -n true >/dev/null 2>&1; then
  echo "passwordless sudo is required for systemctl/nginx checks" >&2
  exit 1
fi

if [[ ! -f .env ]]; then
  echo "missing ${ROOT_DIR}/.env" >&2
  exit 1
fi

if [[ -n "$(git status --porcelain)" ]]; then
  echo "refusing deploy: working tree has uncommitted or untracked changes" >&2
  git status --short >&2
  exit 1
fi

echo "deploy: fetching origin/${BRANCH}"
git fetch --prune origin "${BRANCH}"

current_branch="$(git branch --show-current)"
if [[ "${current_branch}" != "${BRANCH}" ]]; then
  git checkout "${BRANCH}"
fi

git pull --ff-only origin "${BRANCH}"

set -a
# shellcheck disable=SC1091
source .env
set +a

APP_HOST="${APP_HOST:-127.0.0.1}"
APP_PORT="${APP_PORT:-18080}"
PUBLIC_URL="${PUBLIC_URL:-https://vix.micutu.com}"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
binary="${BUILD_DIR}/vix-arena"
backup_dir="${ROOT_DIR}/.deploy-backups"
backup_binary=""
restart_attempted=false
deploy_ok=false

mkdir -p "${backup_dir}"
if [[ -x "${binary}" ]]; then
  backup_binary="${backup_dir}/vix-arena-$(date -u +%Y%m%d%H%M%S)"
  cp -p "${binary}" "${backup_binary}"
fi

rollback() {
  if [[ "${deploy_ok}" == "true" || "${restart_attempted}" != "true" || -z "${backup_binary}" || ! -x "${backup_binary}" ]]; then
    return
  fi

  echo "deploy failed after restart; restoring previous binary" >&2
  cp -p "${backup_binary}" "${binary}"
  sudo -n systemctl restart "${SERVICE}" || true
}
trap rollback EXIT

if [[ "${RUN_CI}" == "true" ]]; then
  echo "deploy: running full VPS CI"
  if [[ -f package-lock.json ]]; then
    require_cmd npm
    npm ci
  fi
  BUILD_DIR="${BUILD_DIR}" BUILD_TYPE="${BUILD_TYPE}" ./scripts/ci.sh
else
  echo "deploy: building release binary"
  BUILD_DIR="${BUILD_DIR}" BUILD_TYPE="${BUILD_TYPE}" ./scripts/build.sh
  ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

if [[ ! -x "${binary}" ]]; then
  echo "missing deploy binary: ${binary}" >&2
  exit 1
fi

echo "deploy: validating nginx config"
sudo -n nginx -t

echo "deploy: restarting ${SERVICE}"
sudo -n systemctl daemon-reload
sudo -n systemctl restart "${SERVICE}"
restart_attempted=true

for _ in {1..60}; do
  if curl -fsS "http://${APP_HOST}:${APP_PORT}/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.5
done

curl -fsS "http://${APP_HOST}:${APP_PORT}/health" | grep -q '"status":"ok"'
curl -fsS "http://${APP_HOST}:${APP_PORT}/api/state" | grep -q '"hazards"'

if ! ss -ltn "sport = :${APP_PORT}" | grep -q "${APP_HOST}:${APP_PORT}"; then
  echo "service is not listening on ${APP_HOST}:${APP_PORT}" >&2
  ss -ltn "sport = :${APP_PORT}" >&2 || true
  exit 1
fi

if [[ "${APP_HOST}" == "127.0.0.1" ]] && ss -ltn "sport = :${APP_PORT}" | grep -q "0.0.0.0:${APP_PORT}"; then
  echo "service is unexpectedly listening on 0.0.0.0:${APP_PORT}" >&2
  exit 1
fi

domain="${PUBLIC_URL#https://}"
domain="${domain#http://}"
domain="${domain%%/*}"
if [[ -n "${domain}" ]]; then
  curl -fsS --resolve "${domain}:443:127.0.0.1" --max-time 8 "https://${domain}/health" | grep -q '"status":"ok"'
fi

if command -v node >/dev/null 2>&1 && [[ -d node_modules/ws ]]; then
  node <<'NODE'
const WebSocket = require("ws");
const publicUrl = process.env.PUBLIC_URL || "https://vix.micutu.com";
const domain = new URL(publicUrl).hostname;
const ws = new WebSocket("wss://127.0.0.1/ws", {
  rejectUnauthorized: false,
  servername: domain,
  headers: { Host: domain, Origin: publicUrl }
});
const finish = (code, message) => {
  if (message) console.log(message);
  try { ws.close(); } catch {}
  process.exit(code);
};
ws.on("open", () => {
  ws.send(JSON.stringify({
    type: "join",
    name: "DeployCheck",
    room: "deploy-check",
    protocolVersion: 2,
    supports: ["snapshot_delta"]
  }));
});
ws.on("message", (data) => {
  const msg = JSON.parse(data);
  if (msg.type === "snapshot" && Array.isArray(msg.hazards) && msg.hazards.length > 0) {
    finish(0, `wss check ok: hazards=${msg.hazards.length}`);
  }
});
ws.on("error", (error) => finish(1, `wss check failed: ${error.message}`));
setTimeout(() => finish(1, "wss check timed out"), 5000);
NODE
else
  echo "deploy: skipping WSS check because node/ws is unavailable"
fi

sudo -n systemctl --no-pager --plain status "${SERVICE}" | sed -n '1,14p'
deploy_ok=true
echo "deploy ok: ${SERVICE} running on ${APP_HOST}:${APP_PORT}"
