#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

fail() {
  echo "security audit failed: $*" >&2
  exit 1
}

require_text() {
  local pattern="$1"
  local path="$2"
  rg -q "${pattern}" "${path}" || fail "missing '${pattern}' in ${path}"
}

require_absent() {
  local pattern="$1"
  local path="$2"
  if rg -q "${pattern}" "${path}"; then
    fail "unexpected '${pattern}' in ${path}"
  fi
}

if command -v git >/dev/null 2>&1 && git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  git check-ignore -q .env || fail ".env is not ignored by git"
else
  rg -q '(^|/)\.env($|[[:space:]])' .gitignore || fail ".env is not listed in .gitignore"
fi

if [[ -f .env ]]; then
  mode="$(stat -c '%a' .env)"
  [[ "${mode}" == "600" || "${mode}" == "400" ]] || fail ".env permissions are ${mode}, expected 600 or 400"
fi

require_text 'APP_HOST=127\.0\.0\.1' README.md
require_text 'ALLOW_MISSING_ORIGIN=false' README.md
require_text 'WebSocket payloads are capped at 4096 bytes' public/docs.html
require_text 'Content-Security-Policy' src/main.cpp
require_text 'cspConnectSrc' src/main.cpp
require_text 'x_frame_options' src/main.cpp
require_text 'X-Content-Type-Options' src/main.cpp
require_text 'Referrer-Policy' src/main.cpp
require_text 'Permissions-Policy' src/main.cpp
require_text 'maxWsPayloadBytes' src/GameServer.cpp
require_text 'allowMissingOrigin' src/main.cpp
require_text 'ALLOW_MISSING_ORIGIN", false' src/main.cpp
require_text 'HttpRateLimiter' src/main.cpp
require_text 'MAX_ACTIVE_ROOMS=128' README.md
require_text 'MAX_ACTIVE_ROOMS' src/main.cpp
require_text 'canCreateRoomLocked' src/GameServer.cpp
require_text 'pruneEmptyRoomLocked' src/GameServer.cpp
require_text 'token bucket' README.md
require_text 'NoNewPrivileges=true' systemd/vix-arena.service.example
require_text 'ProtectSystem=strict' systemd/vix-arena.service.example
require_text 'ReadWritePaths=/home/micu/vix/data' systemd/vix-arena.service.example
require_text 'CapabilityBoundingSet=' systemd/vix-arena.service.example
require_text 'SystemCallFilter=@system-service' systemd/vix-arena.service.example
require_text 'pathInside' src/main.cpp
require_text 'FROM debian:bookworm-slim AS build' Dockerfile
require_text 'FROM debian:bookworm-slim AS runtime' Dockerfile
require_text 'COPY --from=build /app/build/vix-arena' Dockerfile
require_text '^\.env\.\*$' .dockerignore
require_text '^data$' .dockerignore
require_text '^node_modules$' .dockerignore
require_text 'USER 10001:10001' Dockerfile
require_text 'read_only: true' docker-compose.yml
require_text 'no-new-privileges:true' docker-compose.yml
require_text 'cap_drop:' docker-compose.yml

require_absent 'APP_HOST=0\.0\.0\.0' systemd/vix-arena.service.example
require_absent "connect-src 'self' ws: wss:" src/main.cpp
require_absent 'innerHTML' public/app.js public/stats.js
require_absent 'password *= *"[^"]+"' src public scripts docs README.md CMakeLists.txt systemd
require_absent 'api[_-]?key *= *"[^"]+"' src public scripts docs README.md CMakeLists.txt systemd
require_absent 'BEGIN (RSA |OPENSSH |EC )?PRIVATE KEY' src public scripts docs README.md CMakeLists.txt systemd

echo "security audit ok"
