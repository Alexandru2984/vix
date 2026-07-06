# VixArena Protocol

Current protocol version: `2`

VixArena uses JSON over WebSocket at `/ws`. The server remains backward compatible with v1 clients that only understand full `snapshot` messages.

## Negotiation

Clients may announce the protocol version and optional features in `join`:

```json
{
  "type": "join",
  "name": "Micu",
  "protocolVersion": 2,
  "supports": ["snapshot_delta"]
}
```

The server answers with:

```json
{
  "type": "welcome",
  "protocolVersion": 2,
  "serverTimeMs": 1779035000000,
  "id": "p-1",
  "resumeToken": "f3a9c2...",
  "features": ["snapshot_delta"],
  "world": {
    "width": 2000,
    "height": 1200,
    "obstacles": []
  }
}
```

## Session Resume

`welcome` carries a per-player `resumeToken`. When a connection drops, the server keeps the player detached (hidden from snapshots and excluded from the simulation, score preserved) for a grace period (`RESUME_GRACE_SECONDS`, default 45). A client reconnecting to the same room may present the token in `join`:

```json
{"type":"join","name":"Micu","room":"public","resume":"f3a9c2..."}
```

If the token matches a detached player in that room, the server reattaches the session and answers with the original player `id`, the same `resumeToken`, and `"resumed": true`. Otherwise the join proceeds as a fresh player with a new token. Tokens are bearer secrets scoped to one player slot; clients should store them locally and never share them.

## Spectators

A client may join a room as a spectator by setting `"spectate": true` in `join`:

```json
{"type":"join","name":"Watcher","room":"public","spectate":true}
```

The server answers with a `welcome` carrying `"spectator": true` and **no** `id` or `resumeToken`. Spectators receive the same full/delta snapshots and room broadcasts (chat, events, join/leave) as players, but they are not part of the simulation: their `input`, `ability`, and `chat` messages are ignored, and they never appear in snapshots. Spectator slots are capped per room by the same `maxPlayersPerRoom` limit; a full room answers with `error` `"too many spectators"`.

A spectator promotes itself to a real player by sending an ordinary `join` (without `spectate`) on the same connection; the server drops the spectator record and spawns a player. Spectator counts are exposed as `spectators` in `/api/stats` and `activeSpectators` in the websocket stats block.

## Client Messages

```json
{"type":"join","name":"Micu","protocolVersion":2,"supports":["snapshot_delta"]}
{"type":"join","name":"Watcher","room":"public","spectate":true}
{"type":"input","up":true,"down":false,"left":false,"right":true,"seq":123}
{"type":"ability","ability":"dash"}
{"type":"ability","ability":"shield"}
{"type":"ability","ability":"magnet"}
{"type":"chat","message":"salut"}
{"type":"ping","t":1710000000000}
{"type":"resync"}
```

`seq` is client-owned and echoed as `inputSeq` in player snapshots. That gives clients a stable reconciliation anchor without trusting client-side movement.

## Full Snapshots

The first state update after join is always a full snapshot:

```json
{
  "type": "snapshot",
  "protocolVersion": 2,
  "snapshotId": 42,
  "baseSnapshotId": null,
  "tick": 1200,
  "full": true,
  "serverTimeMs": 1779035000000,
  "serverTime": "2026-05-17T16:20:00Z",
  "players": [],
  "orbs": [],
  "powerups": [],
  "hazards": [
    {"id":"h-1","x":520,"y":760,"radius":46,"penaltyPerSecond":3,"color":"#ff5c8a"}
  ],
  "controlZone": {"x":1000,"y":600,"radius":150,"pointsPerSecond":2},
  "round": {"number":1,"phase":"active","secondsRemaining":180},
  "events": []
}
```

## Delta Snapshots

Clients that negotiated `snapshot_delta` receive deltas after their first full snapshot when the delta payload is smaller than the full payload:

```json
{
  "type": "snapshot_delta",
  "protocolVersion": 2,
  "snapshotId": 43,
  "baseSnapshotId": 42,
  "tick": 1201,
  "full": false,
  "serverTimeMs": 1779035000050,
  "players": [],
  "removedPlayers": [],
  "orbs": [],
  "removedOrbs": [],
  "powerups": [],
  "removedPowerups": [],
  "hazards": [],
  "removedHazards": [],
  "events": [],
  "removedEvents": []
}
```

Delta arrays contain full replacement objects for changed or new entities. Removed arrays contain entity IDs. `round` and `controlZone` are included only when changed. Hazards are server-authored danger zones; clients should render them but never decide score damage locally.

If a delta would be larger than the full snapshot, the server sends a full snapshot instead and resets that client's baseline.

## Resync

If a client receives a `snapshot_delta` whose `baseSnapshotId` does not match the last snapshot it applied, it must discard the delta and send `{"type":"resync"}`. The server responds with a fresh full `snapshot` and resets that client's delta baseline. Clients should rate limit resync requests (the reference client allows one per 1.5 seconds).

## Compatibility Rules

- Unknown fields must be ignored by clients.
- Unknown message types should be ignored by clients unless the type is `error`.
- v1 clients can omit `protocolVersion` and `supports`; they will continue receiving full `snapshot` messages.
- `serverTimeMs` is Unix epoch milliseconds and is intended for latency and clock-drift measurement.
- `serverTime` is retained as a readable ISO timestamp for compatibility and debugging.

## Observability

Protocol traffic is exposed in `/api/stats` and `/metrics`, including message counts, bytes, full snapshots, delta snapshots, rejected messages, and rate-limit rejections.
