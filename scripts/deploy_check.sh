#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ -f .env ]]; then
  set -a
  # shellcheck disable=SC1091
  source .env
  set +a
fi

APP_HOST="${APP_HOST:-127.0.0.1}"
APP_PORT="${APP_PORT:-18080}"
PUBLIC_URL="${PUBLIC_URL:-https://vix.micutu.com}"

echo "local health:"
curl -fsS "http://${APP_HOST}:${APP_PORT}/health"
echo

echo "local html:"
curl -fsSI "http://${APP_HOST}:${APP_PORT}/" | sed -n '1,8p'

echo "public health:"
curl -fsS "${PUBLIC_URL}/health"
echo

echo "public html:"
curl -fsSI "${PUBLIC_URL}/" | sed -n '1,12p'
