const fmt = new Intl.NumberFormat("en-US", { maximumFractionDigits: 1 });

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
  text("leaderboardUpdated", `Updated ${timeLabel(data.updatedAt)}`);
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
  text("matchesUpdated", `Updated ${timeLabel(data.updatedAt)}`);
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
    span.textContent = timeLabel(match.endedAt);
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
      getJson("/api/leaderboard"),
      getJson("/api/matches"),
    ]);
    renderStats(stats);
    renderLeaderboard(leaderboard);
    renderMatches(matches);
  } catch (error) {
    text("leaderboardUpdated", error.message);
    text("matchesUpdated", error.message);
  }
}

refresh();
setInterval(refresh, 10000);
