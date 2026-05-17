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
  "features": ["snapshot_delta"],
  "world": {
    "width": 2000,
    "height": 1200,
    "obstacles": []
  }
}
```

## Client Messages

```json
{"type":"join","name":"Micu","protocolVersion":2,"supports":["snapshot_delta"]}
{"type":"input","up":true,"down":false,"left":false,"right":true,"seq":123}
{"type":"ability","ability":"dash"}
{"type":"ability","ability":"shield"}
{"type":"ability","ability":"magnet"}
{"type":"chat","message":"salut"}
{"type":"ping","t":1710000000000}
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
  "events": [],
  "removedEvents": []
}
```

Delta arrays contain full replacement objects for changed or new entities. Removed arrays contain entity IDs. `round` and `controlZone` are included only when changed.

If a delta would be larger than the full snapshot, the server sends a full snapshot instead and resets that client's baseline.

## Compatibility Rules

- Unknown fields must be ignored by clients.
- Unknown message types should be ignored by clients unless the type is `error`.
- v1 clients can omit `protocolVersion` and `supports`; they will continue receiving full `snapshot` messages.
- `serverTimeMs` is Unix epoch milliseconds and is intended for latency and clock-drift measurement.
- `serverTime` is retained as a readable ISO timestamp for compatibility and debugging.

## Observability

Protocol traffic is exposed in `/api/stats` and `/metrics`, including message counts, bytes, full snapshots, delta snapshots, rejected messages, and rate-limit rejections.
