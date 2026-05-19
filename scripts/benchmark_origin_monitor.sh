#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVICE="${VIX_SERVICE:-vix-arena.service}"
ENV_FILE="${ROOT_DIR}/.env"
INTERVAL_SECONDS="${MONITOR_INTERVAL_SECONDS:-2}"
DURATION_SECONDS="${MONITOR_DURATION_SECONDS:-0}"
OUT_FILE="${MONITOR_OUT_FILE:-${ROOT_DIR}/benchmark-results/origin-monitor-$(date -u +%Y%m%dT%H%M%SZ).jsonl}"

set -a
# shellcheck disable=SC1090
[[ -f "${ENV_FILE}" ]] && source "${ENV_FILE}"
set +a

APP_HOST="${APP_HOST:-127.0.0.1}"
APP_PORT="${APP_PORT:-18080}"
mkdir -p "$(dirname "${OUT_FILE}")"

timestamp_ms() {
  date -u +%s%3N
}

service_main_pid() {
  systemctl show "${SERVICE}" --property=MainPID --value 2>/dev/null || echo 0
}

collect_once() {
  local now pid service_state stats ps_line rss_kb pcpu pmem threads
  now="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  pid="$(service_main_pid)"
  service_state="$(systemctl show "${SERVICE}" --property=ActiveState --property=SubState --property=NRestarts --property=MemoryCurrent --value 2>/dev/null | paste -sd ',' - || true)"
  stats="$(curl -fsS --max-time 1 "http://${APP_HOST}:${APP_PORT}/api/stats" 2>/dev/null || true)"
  rss_kb=0
  pcpu=0
  pmem=0
  threads=0
  if [[ "${pid}" =~ ^[0-9]+$ && "${pid}" -gt 0 ]]; then
    ps_line="$(ps -p "${pid}" -o rss=,pcpu=,pmem=,nlwp= 2>/dev/null | awk '{$1=$1; print}' || true)"
    if [[ -n "${ps_line}" ]]; then
      read -r rss_kb pcpu pmem threads <<<"${ps_line}"
    fi
  fi

  if [[ -z "${stats}" ]]; then
    stats="null"
  fi

  printf '{"timestamp":"%s","timestampMs":%s,"service":"%s","pid":%s,"rssKb":%s,"cpuPercent":%s,"memPercent":%s,"threads":%s,"appStats":%s}\n' \
    "${now}" \
    "$(timestamp_ms)" \
    "$(printf '%s' "${service_state}" | sed 's/"/\\"/g')" \
    "${pid:-0}" \
    "${rss_kb:-0}" \
    "${pcpu:-0}" \
    "${pmem:-0}" \
    "${threads:-0}" \
    "${stats}"
}

end_at=0
if [[ "${DURATION_SECONDS}" =~ ^[0-9]+$ && "${DURATION_SECONDS}" -gt 0 ]]; then
  end_at=$((SECONDS + DURATION_SECONDS))
fi

while true; do
  collect_once >>"${OUT_FILE}"
  if [[ "${end_at}" -gt 0 && "${SECONDS}" -ge "${end_at}" ]]; then
    break
  fi
  sleep "${INTERVAL_SECONDS}"
done
