#!/usr/bin/env bash
set -euo pipefail

start="${1:-18080}"
host="${2:-127.0.0.1}"
limit="${3:-250}"

if ! [[ "$start" =~ ^[0-9]+$ ]]; then
  echo "start port must be numeric" >&2
  exit 2
fi

is_listening() {
  local port="$1"

  if command -v ss >/dev/null 2>&1; then
    ss -H -ltn "sport = :$port" | grep -q .
    return
  fi

  if command -v python3 >/dev/null 2>&1; then
    python3 - "$host" "$port" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(0.2)
    sys.exit(0 if sock.connect_ex((host, port)) == 0 else 1)
PY
    return
  fi

  (echo >"/dev/tcp/${host}/${port}") >/dev/null 2>&1
}

for ((port=start; port<start+limit; port++)); do
  if ! is_listening "$port"; then
    echo "$port"
    exit 0
  fi
done

echo "no free port found from $start to $((start + limit - 1)) on $host" >&2
exit 1
