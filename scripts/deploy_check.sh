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
if ! curl -fsSI "http://${APP_HOST}:${APP_PORT}/" | grep -qi "x-content-type-options: nosniff"; then
  echo "local security header check failed; running service may not have the hardened build deployed yet" >&2
fi

echo "local metrics:"
if ! curl -fsS "http://${APP_HOST}:${APP_PORT}/metrics" | sed -n '1,12p'; then
  echo "local metrics check failed; the running service may not have the new /metrics build deployed yet" >&2
fi
if ! curl -fsS "http://${APP_HOST}:${APP_PORT}/metrics" | grep -q "vix_arena_postgres_enabled"; then
  echo "local postgres metrics check failed; running service may not have the PostgreSQL build deployed yet" >&2
fi
echo

echo "public health:"
if ! curl -fsS "${PUBLIC_URL}/health"; then
  echo "public health check failed; Cloudflare or public routing may be blocking non-browser requests" >&2
fi
echo

echo "public html:"
if ! curl -fsSI "${PUBLIC_URL}/" | sed -n '1,12p'; then
  echo "public html check failed; Cloudflare or public routing may be blocking non-browser requests" >&2
fi
