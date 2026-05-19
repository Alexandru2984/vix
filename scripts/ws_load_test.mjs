import WebSocket from "ws";

const baseUrl = process.env.BASE_URL ?? "http://127.0.0.1:18080";
const clients = Number.parseInt(process.env.VIX_LOAD_CLIENTS ?? "16", 10);
const durationMs = Number.parseInt(process.env.VIX_LOAD_DURATION_MS ?? "5000", 10);
const inputEveryMs = Number.parseInt(process.env.VIX_LOAD_INPUT_EVERY_MS ?? "100", 10);
const room = process.env.VIX_LOAD_ROOM ?? `load-${Date.now().toString(36)}`;
const rooms = Math.max(1, Number.parseInt(process.env.VIX_LOAD_ROOMS ?? "1", 10));

const wsUrl = baseUrl.replace(/^http:/, "ws:").replace(/^https:/, "wss:").replace(/\/$/, "") + "/ws";
const origin = baseUrl.replace(/\/$/, "");

function wait(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function roomFor(index) {
  if (rooms <= 1) return room;
  return `${room}-${(index % rooms) + 1}`;
}

function connectClient(index) {
  return new Promise((resolve, reject) => {
    const state = {
      index,
      opened: false,
      welcomed: false,
      snapshots: 0,
      pongs: 0,
      errors: 0,
      closes: 0,
      bytes: 0,
      latencies: [],
      ws: null,
      interval: null
    };

    const ws = new WebSocket(wsUrl, { headers: { Origin: origin } });
    state.ws = ws;

    const timeout = setTimeout(() => {
      reject(new Error(`client ${index} timed out connecting`));
      try { ws.close(); } catch {}
    }, 5000);

    ws.on("open", () => {
      clearTimeout(timeout);
      state.opened = true;
      ws.send(JSON.stringify({
        type: "join",
        name: `Load${index}`,
        room: roomFor(index)
      }));

      let seq = 0;
      state.interval = setInterval(() => {
        if (ws.readyState !== WebSocket.OPEN) return;
        const phase = (seq + index) % 4;
        ws.send(JSON.stringify({
          type: "input",
          up: phase === 0,
          down: phase === 2,
          left: phase === 3,
          right: phase === 1,
          seq: ++seq
        }));
        if (seq % 10 === 0) {
          ws.send(JSON.stringify({ type: "ping", t: Date.now() }));
        }
      }, inputEveryMs);
      resolve(state);
    });

    ws.on("message", (data) => {
      state.bytes += data.length ?? String(data).length;
      let msg;
      try {
        msg = JSON.parse(String(data));
      } catch {
        state.errors += 1;
        return;
      }
      if (msg.type === "welcome") state.welcomed = true;
      if (msg.type === "snapshot" || msg.type === "snapshot_delta") state.snapshots += 1;
      if (msg.type === "pong") {
        state.pongs += 1;
        if (typeof msg.t === "number") state.latencies.push(Math.max(0, Date.now() - msg.t));
      }
      if (msg.type === "error") state.errors += 1;
    });

    ws.on("close", () => {
      state.closes += 1;
      if (state.interval) clearInterval(state.interval);
    });

    ws.on("error", () => {
      state.errors += 1;
    });
  });
}

function percentile(values, p) {
  if (!values.length) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const index = Math.min(sorted.length - 1, Math.ceil((p / 100) * sorted.length) - 1);
  return sorted[index];
}

const states = await Promise.all(Array.from({ length: clients }, (_, index) => connectClient(index)));
await wait(durationMs);

for (const state of states) {
  if (state.interval) clearInterval(state.interval);
  state.ws?.close();
}

await wait(500);

const totals = states.reduce((acc, state) => {
  acc.welcomed += state.welcomed ? 1 : 0;
  acc.snapshots += state.snapshots;
  acc.pongs += state.pongs;
  acc.errors += state.errors;
  acc.bytes += state.bytes;
  acc.latencies.push(...state.latencies);
  return acc;
}, { welcomed: 0, snapshots: 0, pongs: 0, errors: 0, bytes: 0, latencies: [] });

const failures = [];
if (totals.welcomed !== clients) failures.push(`welcomed ${totals.welcomed}/${clients}`);
if (totals.snapshots < clients) failures.push(`snapshots ${totals.snapshots}/${clients}`);
if (totals.errors > 0) failures.push(`protocol/client errors ${totals.errors}`);

try {
  const stats = await fetch(`${baseUrl.replace(/\/$/, "")}/api/stats`).then((res) => res.json());
  if (Number(stats.players ?? 0) > clients) {
    failures.push(`unexpected players after close: ${stats.players}`);
  }
} catch (error) {
  failures.push(`stats fetch failed: ${error.message}`);
}

const summary = {
  clients,
  room,
  rooms,
  durationMs,
  welcomed: totals.welcomed,
  snapshots: totals.snapshots,
  pongs: totals.pongs,
  bytes: totals.bytes,
  latencyMs: {
    p50: percentile(totals.latencies, 50),
    p95: percentile(totals.latencies, 95),
    max: totals.latencies.length ? Math.max(...totals.latencies) : 0
  }
};

console.log(JSON.stringify(summary, null, 2));

if (failures.length) {
  console.error(`load test failed: ${failures.join("; ")}`);
  process.exit(1);
}

console.log("load test ok");
