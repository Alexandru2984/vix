import WebSocket from "ws";

const baseUrl = process.env.BASE_URL ?? "http://127.0.0.1:18080";
const clients = Number.parseInt(process.env.VIX_LOAD_CLIENTS ?? "16", 10);
const durationMs = Number.parseInt(process.env.VIX_LOAD_DURATION_MS ?? "5000", 10);
const inputEveryMs = Number.parseInt(process.env.VIX_LOAD_INPUT_EVERY_MS ?? "100", 10);
const chatEveryMs = Number.parseInt(process.env.VIX_LOAD_CHAT_EVERY_MS ?? "0", 10);
const abilityEveryMs = Number.parseInt(process.env.VIX_LOAD_ABILITY_EVERY_MS ?? "0", 10);
const rampMs = Number.parseInt(process.env.VIX_LOAD_RAMP_MS ?? "0", 10);
const reconnectEveryMs = Number.parseInt(process.env.VIX_LOAD_RECONNECT_EVERY_MS ?? "0", 10);
const reconnectPercent = Number.parseFloat(process.env.VIX_LOAD_RECONNECT_PERCENT ?? "0");
const connectTimeoutMs = Number.parseInt(process.env.VIX_LOAD_CONNECT_TIMEOUT_MS ?? "5000", 10);
const allowServerErrors = /^(1|true|yes)$/i.test(process.env.VIX_LOAD_ALLOW_SERVER_ERRORS ?? "false");
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

function sendJoin(ws, index) {
  ws.send(JSON.stringify({
    type: "join",
    name: `Load${index}`,
    room: roomFor(index),
    protocolVersion: 2,
    supports: ["snapshot_delta"]
  }));
}

function startTraffic(state) {
  if (state.interval) clearInterval(state.interval);
  let seq = 0;
  let lastChatAt = 0;
  let lastAbilityAt = 0;
  const abilities = ["dash", "shield", "magnet"];

  state.interval = setInterval(() => {
    const ws = state.ws;
    if (!ws || ws.readyState !== WebSocket.OPEN) return;

    const now = Date.now();
    const phase = (seq + state.index) % 4;
    ws.send(JSON.stringify({
      type: "input",
      up: phase === 0,
      down: phase === 2,
      left: phase === 3,
      right: phase === 1,
      seq: ++seq
    }));

    if (seq % 10 === 0) {
      ws.send(JSON.stringify({ type: "ping", t: now }));
    }

    if (chatEveryMs > 0 && now - lastChatAt >= chatEveryMs) {
      lastChatAt = now;
      ws.send(JSON.stringify({ type: "chat", message: `load chat ${state.index}-${seq}` }));
    }

    if (abilityEveryMs > 0 && now - lastAbilityAt >= abilityEveryMs) {
      lastAbilityAt = now;
      ws.send(JSON.stringify({ type: "ability", ability: abilities[(state.index + seq) % abilities.length] }));
    }
  }, inputEveryMs);
}

function openSocket(state, resolve, reject) {
  const ws = new WebSocket(wsUrl, { headers: { Origin: origin } });
  state.ws = ws;
  state.connectAttempts += 1;

  const timeout = setTimeout(() => {
    state.errors += 1;
    reject?.(new Error(`client ${state.index} timed out connecting`));
    try { ws.close(); } catch {}
  }, connectTimeoutMs);

  ws.on("open", () => {
    clearTimeout(timeout);
    state.opened = true;
    sendJoin(ws, state.index);
    startTraffic(state);
    resolve?.(state);
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
    if (msg.type === "welcome") {
      state.welcomed = true;
      state.welcomedCount += 1;
    }
    if (msg.type === "snapshot" || msg.type === "snapshot_delta") state.snapshots += 1;
    if (msg.type === "pong") {
      state.pongs += 1;
      if (typeof msg.t === "number") state.latencies.push(Math.max(0, Date.now() - msg.t));
    }
    if (msg.type === "error") state.serverErrors += 1;
  });

  ws.on("close", () => {
    clearTimeout(timeout);
    state.closes += 1;
    if (state.interval) {
      clearInterval(state.interval);
      state.interval = null;
    }
  });

  ws.on("error", () => {
    state.errors += 1;
  });
}

function connectClient(index) {
  return new Promise((resolve, reject) => {
    const state = {
      index,
      opened: false,
      welcomed: false,
      welcomedCount: 0,
      connectAttempts: 0,
      snapshots: 0,
      pongs: 0,
      errors: 0,
      serverErrors: 0,
      closes: 0,
      bytes: 0,
      latencies: [],
      ws: null,
      interval: null
    };
    openSocket(state, resolve, reject);
  });
}

function reconnectClient(state) {
  try {
    state.ws?.close();
  } catch {}
  setTimeout(() => {
    openSocket(state);
  }, 50 + (state.index % 10) * 10);
}

function percentile(values, p) {
  if (!values.length) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const index = Math.min(sorted.length - 1, Math.ceil((p / 100) * sorted.length) - 1);
  return sorted[index];
}

const statePromises = [];
const rampDelayMs = clients > 0 && rampMs > 0 ? Math.max(1, Math.floor(rampMs / clients)) : 0;
for (let index = 0; index < clients; index += 1) {
  statePromises.push(connectClient(index));
  if (rampDelayMs > 0) await wait(rampDelayMs);
}

const states = await Promise.all(statePromises);

let reconnectInterval = null;
if (reconnectEveryMs > 0 && reconnectPercent > 0) {
  reconnectInterval = setInterval(() => {
    const count = Math.max(1, Math.ceil((clients * Math.min(100, reconnectPercent)) / 100));
    const start = Math.floor(Math.random() * Math.max(1, clients));
    for (let offset = 0; offset < count; offset += 1) {
      reconnectClient(states[(start + offset) % clients]);
    }
  }, reconnectEveryMs);
}

await wait(durationMs);

if (reconnectInterval) clearInterval(reconnectInterval);

for (const state of states) {
  if (state.interval) clearInterval(state.interval);
  state.ws?.close();
}

await wait(500);

const totals = states.reduce((acc, state) => {
  acc.welcomed += state.welcomed ? 1 : 0;
  acc.connectAttempts += state.connectAttempts;
  acc.welcomedCount += state.welcomedCount;
  acc.snapshots += state.snapshots;
  acc.pongs += state.pongs;
  acc.errors += state.errors;
  acc.serverErrors += state.serverErrors;
  acc.closes += state.closes;
  acc.bytes += state.bytes;
  acc.latencies.push(...state.latencies);
  return acc;
}, { welcomed: 0, welcomedCount: 0, connectAttempts: 0, snapshots: 0, pongs: 0, errors: 0, serverErrors: 0, closes: 0, bytes: 0, latencies: [] });

const failures = [];
if (totals.welcomed !== clients) failures.push(`welcomed ${totals.welcomed}/${clients}`);
if (totals.snapshots < clients) failures.push(`snapshots ${totals.snapshots}/${clients}`);
if (totals.errors > 0) failures.push(`protocol/client errors ${totals.errors}`);
if (!allowServerErrors && totals.serverErrors > 0) failures.push(`server errors ${totals.serverErrors}`);

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
  inputEveryMs,
  chatEveryMs,
  abilityEveryMs,
  rampMs,
  reconnectEveryMs,
  reconnectPercent,
  welcomed: totals.welcomed,
  welcomedCount: totals.welcomedCount,
  connectAttempts: totals.connectAttempts,
  snapshots: totals.snapshots,
  pongs: totals.pongs,
  closes: totals.closes,
  serverErrors: totals.serverErrors,
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
