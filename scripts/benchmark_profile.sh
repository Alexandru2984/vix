#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVICE="${VIX_SERVICE:-vix-arena.service}"
ENV_FILE="${ROOT_DIR}/.env"

usage() {
  cat <<'EOF'
Usage:
  scripts/benchmark_profile.sh enable <source-ip>
  scripts/benchmark_profile.sh disable
  scripts/benchmark_profile.sh status

Environment overrides for enable:
  BENCHMARK_MAX_CONNECTIONS_PER_IP              default: 256
  BENCHMARK_MAX_PLAYERS_PER_ROOM                default: 256
  BENCHMARK_WS_MESSAGE_BURST                    default: 240
  BENCHMARK_WS_MESSAGE_REFILL_PER_SECOND        default: 120
  BENCHMARK_HTTP_RATE_LIMIT_BURST               default: 20000
  BENCHMARK_HTTP_RATE_LIMIT_REFILL_PER_SECOND   default: 10000
EOF
}

set_env() {
  local key="$1"
  local value="$2"
  local tmp
  tmp="$(mktemp)"
  if [[ -f "${ENV_FILE}" ]]; then
    awk -v key="${key}" -v value="${value}" '
      BEGIN { found = 0 }
      $0 ~ "^" key "=" {
        print key "=" value
        found = 1
        next
      }
      { print }
      END {
        if (!found) print key "=" value
      }
    ' "${ENV_FILE}" >"${tmp}"
  else
    printf '%s=%s\n' "${key}" "${value}" >"${tmp}"
  fi
  mv "${tmp}" "${ENV_FILE}"
}

restart_and_verify() {
  cd "${ROOT_DIR}"
  sudo -n nginx -t
  sudo -n systemctl restart "${SERVICE}"

  set -a
  # shellcheck disable=SC1091
  source "${ENV_FILE}"
  set +a

  local host="${APP_HOST:-127.0.0.1}"
  local port="${APP_PORT:-18080}"
  for _ in {1..60}; do
    if curl -fsS "http://${host}:${port}/health" >/dev/null 2>&1; then
      break
    fi
    sleep 0.5
  done

  curl -fsS "http://${host}:${port}/health"
  echo
  curl -fsS "http://${host}:${port}/api/stats" | jq '.websocket | {
    maxConnectionsPerIp,
    benchmarkSourceIps,
    benchmarkMaxConnectionsPerIp,
    benchmarkMessageBurst,
    benchmarkMessageRefillPerSecond,
    rateLimitRejects,
    rejectedConnections
  }'
}

status() {
  cd "${ROOT_DIR}"
  if [[ -f "${ENV_FILE}" ]]; then
    grep -E '^(BENCHMARK_|MAX_CONNECTIONS_PER_IP|WS_MESSAGE_|HTTP_RATE_LIMIT_)' "${ENV_FILE}" || true
  else
    echo "missing ${ENV_FILE}" >&2
  fi
  echo
  set -a
  # shellcheck disable=SC1091
  [[ -f "${ENV_FILE}" ]] && source "${ENV_FILE}"
  set +a
  curl -fsS "http://${APP_HOST:-127.0.0.1}:${APP_PORT:-18080}/api/stats" | jq '.websocket'
}

command="${1:-}"
case "${command}" in
  enable)
    source_ip="${2:-}"
    if [[ -z "${source_ip}" ]]; then
      usage >&2
      exit 2
    fi
    if [[ ! "${source_ip}" =~ ^[0-9A-Fa-f:.]+$ ]]; then
      echo "invalid source IP: ${source_ip}" >&2
      exit 2
    fi
    cp -p "${ENV_FILE}" "${ENV_FILE}.benchmark-backup-$(date -u +%Y%m%d%H%M%S)"
    set_env "BENCHMARK_SOURCE_IPS" "${source_ip}"
    set_env "BENCHMARK_MAX_PLAYERS_PER_ROOM" "${BENCHMARK_MAX_PLAYERS_PER_ROOM:-256}"
    set_env "BENCHMARK_MAX_CONNECTIONS_PER_IP" "${BENCHMARK_MAX_CONNECTIONS_PER_IP:-256}"
    set_env "BENCHMARK_WS_MESSAGE_BURST" "${BENCHMARK_WS_MESSAGE_BURST:-240}"
    set_env "BENCHMARK_WS_MESSAGE_REFILL_PER_SECOND" "${BENCHMARK_WS_MESSAGE_REFILL_PER_SECOND:-120}"
    set_env "BENCHMARK_HTTP_RATE_LIMIT_BURST" "${BENCHMARK_HTTP_RATE_LIMIT_BURST:-20000}"
    set_env "BENCHMARK_HTTP_RATE_LIMIT_REFILL_PER_SECOND" "${BENCHMARK_HTTP_RATE_LIMIT_REFILL_PER_SECOND:-10000}"
    restart_and_verify
    ;;
  disable)
    cp -p "${ENV_FILE}" "${ENV_FILE}.benchmark-backup-$(date -u +%Y%m%d%H%M%S)"
    set_env "BENCHMARK_SOURCE_IPS" ""
    set_env "BENCHMARK_MAX_PLAYERS_PER_ROOM" "256"
    set_env "BENCHMARK_MAX_CONNECTIONS_PER_IP" "128"
    set_env "BENCHMARK_WS_MESSAGE_BURST" "120"
    set_env "BENCHMARK_WS_MESSAGE_REFILL_PER_SECOND" "60"
    set_env "BENCHMARK_HTTP_RATE_LIMIT_BURST" "5000"
    set_env "BENCHMARK_HTTP_RATE_LIMIT_REFILL_PER_SECOND" "1000"
    restart_and_verify
    ;;
  status)
    status
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
