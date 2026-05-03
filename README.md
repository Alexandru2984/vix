# VixArena

VixArena is a lightweight real-time 2D multiplayer spatial sandbox deployed at `https://vix.micutu.com`. It demonstrates a C++ backend handling HTTP routes, WebSocket messaging, an authoritative in-memory game loop, global chat, and a plain browser canvas frontend.

Vix.cpp v2.5.2 was installed and evaluated on this VPS. Its HTTP listener did not honor localhost-only binding in this environment, so the deployed production server uses a Boost.Beast fallback to satisfy the security requirement that the app bind only to `127.0.0.1`.

## Features

- Shared 2D world with server-authoritative movement, bounds, and static obstacles.
- Collectible orb pickups with server-authoritative scoring.
- Temporary speed powerups with server-authoritative boost timers.
- Central control zone that grants passive points while occupied.
- Live leaderboard, arena event feed, minimap, objective HUD, edge target markers, score popups, and local score HUD.
- WebSocket join, input, chat, ping/pong, and snapshot messages.
- 20 ticks/sec server loop with low-cost full snapshots for v1.
- In-memory chat history for the last 50 messages.
- Responsive canvas frontend with interpolation, HUD, chat, mobile chat badge, floating touch joystick, and connection metrics.
- HTTP endpoints for health, sanitized state, stats, docs, and the game page.

## Stack

- C++20
- Vix.cpp SDK v2.5.2 installed on the VPS
- Boost.Beast fallback server for production HTTP/WebSocket binding
- CMake + Ninja
- nlohmann/json
- systemd service: `vix-arena.service`
- Nginx reverse proxy with Certbot TLS

## Build

```bash
cd /home/micu/vix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/home/micu/.local
cmake --build build
```

## Run Locally On The VPS

```bash
cd /home/micu/vix
./build/vix-arena
```

The app reads `.env` from the project root and binds only to localhost.

## Environment Variables

`.env` is intentionally ignored by Git.

```bash
APP_HOST=127.0.0.1
APP_PORT=18080
WS_HOST=127.0.0.1
WS_PORT=18081
PUBLIC_URL=https://vix.micutu.com
```

`APP_PORT` serves HTTP and WebSocket on `/ws`, bound to `APP_HOST`. `WS_PORT` is kept equal to `APP_PORT` for operational clarity. Nginx exposes both under `https://vix.micutu.com`.

## Systemd

Service name: `vix-arena.service`

Installed path:

```bash
/etc/systemd/system/vix-arena.service
```

Local example:

```bash
systemd/vix-arena.service.example
```

Common commands:

```bash
sudo systemctl status vix-arena.service
sudo journalctl -u vix-arena.service -n 100 --no-pager
sudo systemctl restart vix-arena.service
```

## Nginx

Config path:

```bash
/etc/nginx/sites-available/vix.micutu.com
```

Enabled path:

```bash
/etc/nginx/sites-enabled/vix.micutu.com
```

Nginx proxies `/` and `/ws` to `APP_PORT`; `/ws` includes WebSocket upgrade headers and long proxy timeouts.

## Public URL

- Game: `https://vix.micutu.com`
- WebSocket: `wss://vix.micutu.com/ws`
- Docs: `https://vix.micutu.com/docs`

## WebSocket Protocol

Client messages:

```json
{"type":"join","name":"Micu"}
{"type":"input","up":true,"down":false,"left":false,"right":true,"seq":123}
{"type":"chat","message":"salut"}
{"type":"ping","t":1710000000000}
```

Server messages:

```json
{"type":"welcome","id":"p-1","world":{"width":2000,"height":1200,"obstacles":[]}}
{"type":"snapshot","players":[{"id":"p-1","name":"Micu","x":100,"y":200,"color":"#66ccff","score":10,"boostMs":0}],"orbs":[{"id":"o-1","x":400,"y":300,"value":5,"color":"#66ccff"}],"powerups":[{"id":"u-1","kind":"speed","x":700,"y":350,"durationSeconds":6,"color":"#c9a7ff"}],"controlZone":{"x":1000,"y":600,"radius":150,"pointsPerSecond":2},"round":{"number":1,"phase":"active","secondsRemaining":180},"events":[{"id":1,"type":"orb","text":"Micu collected +5","timestamp":"2026-05-03T17:00:00Z"}]}
{"type":"chat","from":"Micu","message":"salut","timestamp":"2026-05-03T17:00:00Z"}
{"type":"player_joined","id":"p-1","name":"Micu"}
{"type":"player_left","id":"p-1"}
{"type":"pong","t":1710000000000}
```

## API Endpoints

- `GET /health` returns status, service name, player count, and uptime.
- `GET /api/state` returns player count, world size, obstacles, current orbs, speed powerups, round state, and control zone metadata.
- `GET /api/stats` returns connected players, uptime, tick target, total connections, total chat messages, orb pickups, powerup pickups, rounds, and control-zone points.
- `GET /docs` explains controls, protocol, endpoints, and limitations.
- `GET /` serves the browser game.

## Controls

- `WASD` or arrow keys: move.
- Touch joystick: move on mobile/touchscreen devices. On touchscreens, dragging directly on the arena also starts a floating joystick.
- Collect glowing orbs for instant points.
- Grab violet boosts for temporary speed.
- Hold the central control zone for passive points.
- Follow the objective HUD and edge markers to find nearby orbs, boosts, or the control zone.
- `Enter`: focus chat.
- `Esc`: unfocus chat.

## Deployment Notes

- Work is contained in `/home/micu/vix`.
- The app binds to `127.0.0.1` only.
- Vix.cpp v2.5.2 was installed and tested, but its HTTP listener did not honor localhost-only binding on this VPS. The production server therefore uses a Boost.Beast fallback while preserving the requested C++ real-time app behavior.
- Nginx is the public reverse proxy.
- Certbot manages the public TLS certificate.
- Existing services should not be stopped or killed.
- If a port is occupied, use `scripts/find_free_port.sh` and update `.env`.

## Troubleshooting

```bash
curl -fsS http://127.0.0.1:${APP_PORT}/health
curl -fsSI https://vix.micutu.com/
sudo nginx -t
sudo systemctl status nginx
sudo systemctl status vix-arena.service
sudo journalctl -u vix-arena.service -n 100 --no-pager
ss -ltnp | grep vix-arena
```

## Security Notes

- No secrets are exposed to the frontend.
- Display names and chat messages are length-limited and control-character cleaned.
- Invalid JSON and unknown WebSocket message types are rejected.
- Chat is throttled per connection.
- Movement is authoritative and clamped server-side.
- No shell commands, user file paths, database, or arbitrary code execution are exposed.
- Nginx adds basic security headers and handles TLS.

## Performance Notes

- State is in memory and intentionally small.
- The target tick rate is 20 ticks/sec.
- Snapshots are compact JSON and include players, scores, boost timers, active orbs, powerups, round state, event feed, and control-zone metadata.
- The design should handle 20-50 connected players for v1 on a small VPS.

## Limitations / TODOs

- No persistence across restarts.
- No authentication or private rooms.
- No binary protocol or delta compression yet.
- Mobile support is intentionally lightweight for v1; advanced gestures, room setup, and landscape-specific UI modes are still TODOs.
- Horizontal scaling would require external state or pub/sub.

## Git

Git commits and pushes are manual. The agent did not run `git add`, `git commit`, or `git push`.
