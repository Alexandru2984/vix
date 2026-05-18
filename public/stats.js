const fmt = new Intl.NumberFormat("en-US", { maximumFractionDigits: 1 });
const roomFilter = document.getElementById("roomFilter");
const applyRoomFilter = document.getElementById("applyRoomFilter");
const clearRoomFilter = document.getElementById("clearRoomFilter");

function text(id, value) {
  const el = document.getElementById(id);
  if (el) el.textContent = value;
}

function timeLabel(value) {
  if (!value) return "never";
  const date = new Date(value);
  return Number.isNaN(date.getTime()) ? value : date.toLocaleString();
}

async function getJson(path) {
  const res = await fetch(path, { cache: "no-store" });
  if (!res.ok) throw new Error(`${path} returned ${res.status}`);
  return res.json();
}

function sanitizeRoom(value) {
  const clean = String(value || "")
    .toLowerCase()
    .replace(/[^a-z0-9_-]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 24);
  return clean.length >= 3 ? clean : "";
}

function selectedRoom() {
  return sanitizeRoom(roomFilter?.value || new URLSearchParams(window.location.search).get("room") || "");
}

function roomQuery() {
  const room = selectedRoom();
  return room ? `?room=${encodeURIComponent(room)}` : "";
}

function setUrlRoom(room) {
  const url = new URL(window.location.href);
  if (room) url.searchParams.set("room", room);
  else url.searchParams.delete("room");
  window.history.replaceState({}, "", url);
}

function renderStats(stats) {
  text("statPlayers", stats.connectedPlayers ?? 0);
  text("statHumans", `${stats.humanPlayers ?? 0} humans, ${stats.botPlayers ?? 0} bots`);
  text("statRounds", stats.totalRoundsCompletedSinceStart ?? 0);
  text("statRoundNumber", `round ${stats.roundNumber ?? 1}`);
  text("statMessages", stats.websocket?.messagesReceived ?? 0);
  text("statRejected", `${stats.websocket?.rejectedMessages ?? 0} rejected`);
  text("statTick", `${stats.tickDurationUs?.p95 ?? 0}us`);
  text("statUptime", `${stats.uptimeSeconds ?? 0}s uptime`);
  const persistence = stats.persistence || {};
  text("statStorage", persistence.postgresEnabled ? "Postgres" : "JSON");
  text("statStorageDetail", persistence.postgresEnabled
    ? `${persistence.postgresSavedMatches ?? 0} saved, ${persistence.postgresQueuedWrites ?? 0} queued`
    : "fallback");
}

function renderLeaderboard(data) {
  const room = data.room ? `Room ${data.room}` : "All rooms";
  text("leaderboardUpdated", `${room} · Updated ${timeLabel(data.updatedAt)}`);
  const body = document.getElementById("leaderboardRows");
  if (!body) return;
  body.replaceChildren();

  const entries = data.entries || [];
  if (entries.length === 0) {
    const row = document.createElement("tr");
    const cell = document.createElement("td");
    cell.colSpan = 6;
    cell.textContent = "No completed human rounds yet.";
    row.append(cell);
    body.append(row);
    return;
  }

  for (const entry of entries) {
    const row = document.createElement("tr");
    for (const value of [
      `#${entry.rank}`,
      entry.name,
      entry.wins,
      entry.rounds,
      entry.bestScore,
      fmt.format(entry.averageScore || 0),
    ]) {
      const cell = document.createElement("td");
      cell.textContent = value;
      row.append(cell);
    }
    body.append(row);
  }
}

function renderMatches(data) {
  const room = data.room ? `Room ${data.room}` : "All rooms";
  text("matchesUpdated", `${room} · Updated ${timeLabel(data.updatedAt)}`);
  const list = document.getElementById("matchRows");
  if (!list) return;
  list.replaceChildren();

  const matches = data.matches || [];
  if (matches.length === 0) {
    const empty = document.createElement("p");
    empty.className = "empty-state";
    empty.textContent = "No match history recorded yet.";
    list.append(empty);
    return;
  }

  for (const match of matches) {
    const item = document.createElement("article");
    item.className = "match-row";

    const title = document.createElement("div");
    const strong = document.createElement("strong");
    strong.textContent = `Round ${match.round}`;
    const span = document.createElement("span");
    span.textContent = `${match.room || "public"} · ${timeLabel(match.endedAt)}`;
    title.append(strong, span);

    const result = document.createElement("p");
    const winner = match.winner || {};
    result.textContent = `${winner.name || "No winner"} won with ${winner.score ?? 0} points`;

    item.append(title, result);
    list.append(item);
  }
}

async function refresh() {
  try {
    const [stats, leaderboard, matches] = await Promise.all([
      getJson("/api/stats"),
      getJson(`/api/leaderboard${roomQuery()}`),
      getJson(`/api/matches${roomQuery()}`),
    ]);
    renderStats(stats);
    renderLeaderboard(leaderboard);
    renderMatches(matches);
  } catch (error) {
    text("leaderboardUpdated", error.message);
    text("matchesUpdated", error.message);
  }
}

const initialRoom = sanitizeRoom(new URLSearchParams(window.location.search).get("room") || "");
if (roomFilter && initialRoom) roomFilter.value = initialRoom;
applyRoomFilter?.addEventListener("click", () => {
  const room = selectedRoom();
  if (roomFilter) roomFilter.value = room;
  setUrlRoom(room);
  refresh();
});
clearRoomFilter?.addEventListener("click", () => {
  if (roomFilter) roomFilter.value = "";
  setUrlRoom("");
  refresh();
});
roomFilter?.addEventListener("keydown", (event) => {
  if (event.key === "Enter") applyRoomFilter?.click();
});
roomFilter?.addEventListener("input", () => {
  roomFilter.value = sanitizeRoom(roomFilter.value);
});

refresh();
setInterval(refresh, 10000);
