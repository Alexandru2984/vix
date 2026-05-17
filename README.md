# VixArena

VixArena is a real-time 2D multiplayer arena deployed at `https://vix.micutu.com`. The backend is a C++20 Boost.Beast HTTP/WebSocket server with an authoritative in-memory game loop, bots, abilities, chat, objectives, and a browser canvas frontend.

The production service binds to `127.0.0.1` and is exposed through Nginx and Cloudflare.

## Quick Start

```bash
git clone <repo-url> vix
cd vix
./scripts/check.sh
```

`scripts/check.sh` builds the project, runs the CTest suite, starts a temporary local server on a free localhost port, and verifies `/health`, `/api/state`, `/api/stats`, `/api/leaderboard`, `/api/matches`, `/metrics`, `/`, `/docs`, `/stats`, security headers, and WebSocket Origin rejection.

To build only:

```bash
./scripts/build.sh
```

The build script uses Ninja when available and falls back to Unix Makefiles. If an old CMake cache was configured with a generator that is not available on the machine, the script reconfigures the build directory.

## Run Locally

```bash
./scripts/build.sh
APP_HOST=127.0.0.1 APP_PORT=18080 ./build/vix-arena
```

Then open:

```text
http://127.0.0.1:18080
```

## Docker

```bash
docker compose up --build
```

Then open:

```text
http://127.0.0.1:18080
```

The container binds to `0.0.0.0` internally and publishes port `18080` by default. Override the host port with:

```bash
APP_PORT=18082 docker compose up --build
```

## CMake Presets

```bash
cmake --preset release
cmake --build --preset release
```

Available presets:

- `release`: Unix Makefiles, `build/`
- `debug`: Unix Makefiles, `build-debug/`
- `ninja-release`: Ninja, `build-ninja/`

## Tests

```bash
./scripts/check.sh
```

For the C++ unit suite only:

```bash
./scripts/build.sh
ctest --test-dir build --output-on-failure
```

The current unit tests cover text validation, world bounds/collision/spawn behavior, protocol serialization, bad WebSocket payload handling, join flow, chat broadcast, rate limiting, and bot cleanup when the last human leaves.

## Environment

The app reads `.env` from the project root. Environment variables override `.env` values.

```bash
APP_HOST=127.0.0.1
APP_PORT=18080
WS_HOST=127.0.0.1
WS_PORT=18081
PUBLIC_URL=https://vix.micutu.com
ALLOWED_ORIGINS=https://vix.micutu.com,http://127.0.0.1:18080,http://localhost:18080
ALLOW_MISSING_ORIGIN=true
DATA_DIR=/home/micu/vix/data
```

`APP_PORT` serves both HTTP and WebSocket traffic. The WebSocket endpoint is `/ws`.
`ALLOWED_ORIGINS` is a comma-separated browser Origin allowlist for WebSocket upgrades. Missing Origin headers are allowed by default for local CLI smoke tests and can be disabled with `ALLOW_MISSING_ORIGIN=false`.
`DATA_DIR` stores the persistent leaderboard and match history in `vix-arena-state.json`.

## Features

- Server-authoritative movement, bounds, obstacles, scoring, pickups, powerups, and abilities.
- 20 ticks/sec authoritative game loop.
- WebSocket protocol for join, input, abilities, chat, ping/pong, and snapshots.
- Bots fill the arena for solo play when humans are connected.
- Contested control zone, Orb Run mini quest, leaderboard, minimap, event feed, objective markers, and score feedback.
- Responsive browser frontend with canvas rendering, interpolation, keyboard controls, touch joystick, chat, HUD, and connection metrics.
- Persistent leaderboard and recent match history stored as JSON on disk.
- Public stats page for runtime counters, leaderboard, and recent matches.
- HTTP endpoints for health, public state, runtime stats, leaderboard, match history, docs, and static frontend files.
- Prometheus-style `/metrics` endpoint with connection, message, tick-duration, gameplay, and rejection counters.

## Stack

- C++20
- Boost.Beast / Boost.Asio
- nlohmann/json
- CMake
- Browser canvas frontend
- systemd service
- Nginx reverse proxy
- Certbot TLS
- Cloudflare in front of the VPS

## Architecture

```text
Browser
  | HTTPS + WSS
Cloudflare
  |
Nginx
  | localhost proxy
vix-arena C++ server
  |-- HTTP routes: /, /stats, /docs, /health, /api/state, /api/stats, /api/leaderboard, /api/matches, /metrics
  |-- WebSocket route: /ws
  |-- authoritative game loop
  |-- in-memory players, bots, pickups, rounds, chat history
```

## WebSocket Protocol

Detailed protocol notes live in [docs/protocol.md](docs/protocol.md).

Client messages:

```json
{"type":"join","name":"Micu","protocolVersion":2,"supports":["snapshot_delta"]}
{"type":"input","up":true,"down":false,"left":false,"right":true,"seq":123}
{"type":"ability","ability":"dash"}
{"type":"ability","ability":"shield"}
{"type":"ability","ability":"magnet"}
{"type":"chat","message":"salut"}
{"type":"ping","t":1710000000000}
```

Server messages:

```json
{"type":"welcome","protocolVersion":2,"serverTimeMs":1779035000000,"id":"p-1","features":["snapshot_delta"],"world":{"width":2000,"height":1200,"obstacles":[]}}
{"type":"snapshot","protocolVersion":2,"snapshotId":1,"baseSnapshotId":null,"tick":1,"full":true,"serverTimeMs":1779035000050,"players":[],"orbs":[],"powerups":[],"controlZone":{"x":1000,"y":600,"radius":150,"pointsPerSecond":2},"round":{"number":1,"phase":"active","secondsRemaining":180},"events":[]}
{"type":"snapshot_delta","protocolVersion":2,"snapshotId":2,"baseSnapshotId":1,"tick":2,"full":false,"serverTimeMs":1779035000100,"players":[],"removedPlayers":[],"orbs":[],"removedOrbs":[],"powerups":[],"removedPowerups":[],"events":[],"removedEvents":[]}
{"type":"chat","from":"Micu","message":"salut","timestamp":"2026-05-03T17:00:00Z"}
{"type":"player_joined","id":"p-1","name":"Micu"}
{"type":"player_left","id":"p-1"}
{"type":"pong","t":1710000000000}
```

## API

- `GET /health`: service status, player counts, uptime.
- `GET /api/state`: public game state, world metadata, pickups, round, events.
- `GET /api/stats`: operational counters.
- `GET /api/leaderboard`: persistent top players sorted by wins, best score, total score, and name.
- `GET /api/matches`: recent persisted round results.
- `GET /metrics`: Prometheus text metrics.
- `GET /stats`: public stats page with leaderboard and match history.
- `GET /docs`: browser documentation page.
- `GET /`: game frontend.

## Observability

`/metrics` returns Prometheus text format:

```bash
curl -fsS http://127.0.0.1:18080/metrics
```

Current metrics include:

- process health and uptime
- current humans, bots, and total connected players
- WebSocket messages and bytes received/sent
- full snapshots, delta snapshots, and snapshot bytes sent
- rejected messages, rate-limit rejections, and send failures
- authoritative tick count and recent tick duration p50/p95/p99/max
- accepted chat messages, pickups, quests, rounds, and control-zone points
- persistent leaderboard and match history entry counts

Startup and fatal startup errors are emitted as single-line JSON logs for systemd/journal ingestion.

## Production

Service name:

```bash
vix-arena.service
```

Common commands:

```bash
sudo systemctl status vix-arena.service
sudo journalctl -u vix-arena.service -n 100 --no-pager
sudo systemctl restart vix-arena.service
```

Deploy verification:

```bash
./scripts/deploy_check.sh
```

`deploy_check.sh` requires local health to pass and reports public Cloudflare/routing issues as warnings.

## Security Notes

- The production app binds only to localhost.
- Nginx handles the public TLS endpoint.
- WebSocket browser Origins are checked against `ALLOWED_ORIGINS` and `PUBLIC_URL`.
- Per-client WebSocket outboxes are capped to avoid unbounded memory growth.
- The app handles `SIGTERM`/`SIGINT` for graceful shutdown under systemd.
- HTTP responses include baseline security headers: CSP, `X-Content-Type-Options`, `X-Frame-Options`, `Referrer-Policy`, and `Permissions-Policy`.
- Display names and chat messages are length-limited and cleaned of control characters.
- WebSocket payload size is capped before JSON parsing.
- Invalid JSON and unknown message types are rejected.
- Chat and input messages are throttled per connection.
- Player movement, ability cooldowns, pickups, and scoring are server-authoritative.
- No shell execution, user file paths, database access, or arbitrary code execution is exposed.

## Current Limitations

- No persistence across restarts.
- No authentication or private rooms.
- No CI until billing is available; use `./scripts/check.sh` locally as the source of truth.
- No binary protocol or delta snapshots yet.
- Horizontal scaling would require external state or pub/sub.
