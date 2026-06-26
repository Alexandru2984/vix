# VixArena Security Audit

Audit date: 2026-05-19

This is a practical security review of the VixArena application, runtime configuration, and deployment surface. It is not a formal penetration test, but it covers the main externally reachable surfaces and the local production setup.

## Scope

- C++ HTTP server and WebSocket protocol handling
- Browser frontend rendering and service worker behavior
- PostgreSQL persistence access patterns
- Nginx reverse proxy configuration for `vix.micutu.com`
- systemd service hardening
- Secrets and generated artifact hygiene
- Benchmark profile behavior

## Summary

No critical or high-severity application issues were found during this pass.

The project already has the important production controls in place:

- service binds to `127.0.0.1:18080`
- Nginx terminates TLS and proxies HTTP/WebSocket traffic
- WebSocket Origin allowlist is enforced
- missing WebSocket Origin headers are rejected in production
- WebSocket payload size is capped at 4096 bytes
- invalid WebSocket messages are counted and closed after repeated abuse
- HTTP dynamic endpoints and WebSocket messages are rate-limited
- chat, display names, and room codes are sanitized/capped
- SQL access uses parameterized `pqxx::exec_params`
- service runs as non-root user `micu`
- systemd sandboxing is enabled
- `.env` is ignored by Git and has `0600` permissions
- `/metrics` and `/ready` are restricted by Nginx to localhost

## Findings Fixed In This Pass

### Capped active private rooms

Severity: medium

Room codes come from client-controlled join messages. The server capped players per room and connections per IP, but it did not cap the total number of active rooms. A distributed or proxy-based attacker could create many low-population private rooms and force unbounded growth in room state, bot state, chat history, and per-tick work.

Fix:

- added `MAX_ACTIVE_ROOMS`, defaulting to `128`
- rejected joins that would create a room above the active-room cap
- pruned empty private rooms and their chat history after the last human leaves
- exposed the active-room cap/count in stats and added regression coverage

### Restricted CSP WebSocket destinations

Severity: low

The app CSP used `connect-src 'self' ws: wss:`, which allowed browser code running in the app origin to open WebSocket connections to any host. The application does not load third-party scripts and did not have an obvious XSS path, but broad `connect-src` makes future injection bugs more useful for exfiltration or pivoting.

Fix:

- replaced wildcard WebSocket schemes with same-authority `ws://<host>` and `wss://<host>` sources
- validates the Host authority before reflecting it into the CSP header
- added an audit check to reject the broad `ws:`/`wss:` directive

### Default-denied missing WebSocket Origin

Severity: low

When `ALLOW_MISSING_ORIGIN` was not configured, the server allowed missing WebSocket Origin headers if `PUBLIC_URL` was empty. That is convenient for ad hoc non-browser clients, but it is risky if an operator exposes a default local/dev configuration directly. Browser cross-site WebSocket attempts include an Origin header, yet non-browser clients can omit it.

Fix:

- changed the default for missing Origin headers to deny
- kept `ALLOW_MISSING_ORIGIN=true` as an explicit opt-in for trusted non-browser clients
- updated documentation to describe the safer default

### Hardened Docker runtime privileges

Severity: medium

The Docker image did not declare a non-root user, so the app process ran as root inside the container. A container escape is not implied by that alone, but root in-container increases blast radius for file writes, mounted volumes, and future runtime misconfiguration.

Fix:

- added a dedicated non-root UID/GID `10001` in the Docker image
- switched Docker Compose persistence to a managed `vix-data` volume
- enabled read-only container filesystem, `no-new-privileges`, `cap_drop: ALL`, and a `/tmp` tmpfs in Compose
- added audit checks for these container hardening controls

### Removed build toolchain from Docker runtime

Severity: low

The Dockerfile built and ran the application in the same image layer, leaving compilers, CMake, headers, and build tools available at runtime. Those tools are not needed by the service and increase post-compromise utility inside the container.

Fix:

- converted Dockerfile to a multi-stage build
- kept CMake, compiler, headers, and tests in the build stage only
- copied only the app binary, public assets, migrations, and required runtime libraries into the runtime stage

### Excluded env backups from Docker build context

Severity: medium

`.dockerignore` excluded `.env` but not `.env.*`. Local benchmark backup files matched `.env.benchmark-backup-*`, so they were ignored by Git but still eligible to be sent to the Docker daemon and copied into the build stage by `COPY . .`.

Fix:

- added `.env.*` to `.dockerignore`
- excluded local data, `node_modules`, Playwright results, test results, and benchmark results from the Docker context
- added audit checks for sensitive Docker context excludes

### Bounded JSON fallback persistence

Severity: medium

The JSON fallback state file was parsed without a size check, and the fallback leaderboard could grow with every unique display name. A corrupted or oversized local state file could make startup expensive, and long-running public abuse could grow JSON persistence unnecessarily.

Fix:

- ignore fallback state files larger than 1 MiB
- cap loaded and saved fallback leaderboard entries at 500
- prune the in-memory fallback leaderboard after round recording
- added unit coverage for oversized state and leaderboard caps

### Validated benchmark source IP before SSH

Severity: medium

`benchmark_one_step.sh` passed `SOURCE_IP` into a remote SSH command before local validation. The remote `benchmark_profile.sh` validates the value, but shell parsing happens first, so a malicious local `SOURCE_IP` value could alter the remote command.

Fix:

- validate `SOURCE_IP` locally before usage
- reject values outside IPv4/IPv6 address characters before any SSH command is built
- added an audit check for the local validation guard

### Removed frontend `innerHTML` usage

Severity: low

The main client had one `innerHTML` assignment for the round-complete banner. The player name was escaped before insertion, so this was not an obvious exploitable XSS. Still, keeping user-controlled values away from HTML string construction is safer and easier to audit.

Fix:

- replaced the `innerHTML` assignment with DOM node construction and `textContent`
- added a security audit check that rejects `innerHTML` in app client scripts

### Hardened static file serving against symlink escape

Severity: low

The HTTP server already rejected suspicious paths such as `..`, `%`, backslashes, and double-slash targets. This protected normal path traversal attempts. However, static file serving did not canonicalize the final filesystem path, so a bad symlink inside `public/` could have pointed outside the static root if such a symlink was ever introduced by an operator or deployment mistake.

Fix:

- added canonical path containment validation before reading static files
- verified that a temporary `public/` symlink to `.env` returns `400` instead of serving content
- added a security audit check for the static root containment helper

## Verified Controls

### WebSocket protocol

- invalid JSON returns an error and increments protocol violation counters
- oversized WebSocket payloads are rejected before parsing
- unknown message types are rejected
- repeated invalid messages close the connection
- per-connection token bucket throttles message spam
- per-IP connection caps are enforced
- benchmark allowlist limits are separate from production defaults
- Origin checks reject bad or missing origins in production

### HTTP/API

- only `GET` and `HEAD` are accepted
- target URI length is capped
- dynamic endpoints are rate-limited
- room query filters are sanitized
- path traversal probes against `.env` return `400` or `404`
- security headers are applied by both app and Nginx

### Persistence

- user-controlled query values use parameterized PostgreSQL queries
- leaderboard and match limits are clamped
- persistence failures do not crash the app; JSON fallback remains available
- migration files are local repo files, not user-controlled input

### Frontend

- chat, leaderboard, events, player names, and match data render through `textContent`
- service worker does not cache `/ws`, `/api/`, `/health`, `/ready`, or `/metrics`
- CSP blocks third-party scripts and framing
- localStorage contains only non-secret UX preferences/name/room

### Deployment/runtime

- live process listens only on `127.0.0.1:18080`
- Nginx has WebSocket upgrade headers and long WS timeouts
- Nginx restricts `/metrics` and `/ready` to localhost
- Nginx uses HSTS and security headers
- Cloudflare real IP configuration exists globally
- systemd hardening exposure score: `1.6 OK`
- no Git commit/push automation is performed by the app scripts

## Checks Run

```bash
./scripts/security_audit.sh
npm audit --omit=dev
./scripts/build.sh && ctest --test-dir build --output-on-failure
./scripts/check.sh
./scripts/e2e.sh
```

Additional targeted checks:

```bash
# Static path traversal probes
curl http://127.0.0.1:18080/../../.env
curl http://127.0.0.1:18080/%2e%2e/%2e%2e/.env
curl http://127.0.0.1:18080//etc/passwd
curl http://127.0.0.1:18080/.env

# WebSocket Origin rejection
curl -H 'Connection: Upgrade' \
  -H 'Upgrade: websocket' \
  -H 'Sec-WebSocket-Version: 13' \
  -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
  -H 'Origin: https://evil.example' \
  http://127.0.0.1:18080/ws
```

Results:

- `security_audit.sh`: passed
- `npm audit --omit=dev`: 0 vulnerabilities
- C++ unit tests: passed
- local HTTP/metrics/security smoke: passed
- Playwright E2E/accessibility/responsive/PWA: 35 passed, 7 skipped
- temporary static symlink escape check: returned `400`
- bad/missing WebSocket Origin checks: returned `403`

## Residual Risks And Hardening TODOs

- Public `/health`, `/api/state`, `/api/stats`, `/api/rooms`, `/api/leaderboard`, and `/api/matches` are intentionally unauthenticated. They expose sanitized game/runtime metadata only, but they remain scrapeable public endpoints.
- Benchmark profile can significantly relax limits for allowlisted IPs. It should stay disabled except during controlled tests.
- `/api/stats` exposes operational counters. This is useful for the public demo, but a stricter production posture could move detailed stats behind localhost/Nginx-only access and keep a smaller public stats payload.
- CSP still allows inline styles because the current frontend uses stylesheet-driven UI plus some browser defaults. A stricter future version could remove `style-src 'unsafe-inline'`.
- There is no authentication model; private rooms are invite-by-link room codes, not access-controlled rooms.
- No WAF-level per-route Nginx `limit_req` is configured for Vix. The app has internal rate limiting, and Cloudflare is in front, but Nginx-side limits could add another layer for `/api/*`.

## Current Security Posture

VixArena is appropriate as a public multiplayer demo behind Cloudflare and Nginx with the current controls. The largest practical risks are abuse/load management and public observability exposure, not obvious code execution, SQL injection, XSS, or secret leakage.
