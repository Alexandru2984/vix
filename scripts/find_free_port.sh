#!/usr/bin/env bash
set -euo pipefail

start="${1:-18080}"
host="${2:-127.0.0.1}"
limit="${3:-250}"

if ! [[ "$start" =~ ^[0-9]+$ ]]; then
  echo "start port must be numeric" >&2
  exit 2
fi

for ((port=start; port<start+limit; port++)); do
  if ! ss -H -ltn "sport = :$port" | grep -q .; then
    echo "$port"
    exit 0
  fi
done

echo "no free port found from $start to $((start + limit - 1)) on $host" >&2
exit 1
