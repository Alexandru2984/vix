(() => {
  const canvas = document.getElementById("arena");
  const ctx = canvas.getContext("2d");
  const statusEl = document.getElementById("status");
  const playersEl = document.getElementById("players");
  const scoreEl = document.getElementById("score");
  const roundTimeEl = document.getElementById("roundTime");
  const boostEl = document.getElementById("boost");
  const pingEl = document.getElementById("ping");
  const joinPanel = document.getElementById("joinPanel");
  const nameInput = document.getElementById("nameInput");
  const joinBtn = document.getElementById("joinBtn");
  const chatLog = document.getElementById("chatLog");
  const chatInput = document.getElementById("chatInput");
  const leaderboardEl = document.getElementById("leaderboard");
  const roundBanner = document.getElementById("roundBanner");
  const eventFeed = document.getElementById("eventFeed");

  const state = {
    ws: null,
    joined: false,
    localId: null,
    world: { width: 2000, height: 1200, obstacles: [] },
    orbs: [],
    powerups: [],
    controlZone: { x: 1000, y: 600, radius: 150, pointsPerSecond: 2 },
    round: { number: 1, phase: "active", secondsRemaining: 180, lastWinner: { name: "No winner yet", score: 0 } },
    events: [],
    seenEvents: new Set(),
    players: new Map(),
    renderPlayers: new Map(),
    keys: { up: false, down: false, left: false, right: false },
    seq: 0,
    lastInputSent: "",
    camera: { x: 1000, y: 600 },
    pingTimer: 0,
    reconnectTimer: 0,
    startedAt: performance.now()
  };

  function resize() {
    const rect = canvas.getBoundingClientRect();
    const dpr = Math.max(1, Math.min(2, window.devicePixelRatio || 1));
    canvas.width = Math.floor(rect.width * dpr);
    canvas.height = Math.floor(rect.height * dpr);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }

  function setStatus(text, online) {
    statusEl.textContent = text;
    statusEl.classList.toggle("online", Boolean(online));
  }

  function wsUrl() {
    const proto = window.location.protocol === "https:" ? "wss:" : "ws:";
    return `${proto}//${window.location.host}/ws`;
  }

  function connect() {
    clearTimeout(state.reconnectTimer);
    setStatus("connecting", false);
    const ws = new WebSocket(wsUrl());
    state.ws = ws;

    ws.addEventListener("open", () => {
      setStatus("online", true);
      if (state.joined) sendJoin();
    });

    ws.addEventListener("close", () => {
      setStatus("offline", false);
      state.reconnectTimer = setTimeout(connect, 1400);
    });

    ws.addEventListener("error", () => setStatus("error", false));

    ws.addEventListener("message", (event) => {
      let msg;
      try {
        msg = JSON.parse(event.data);
      } catch {
        return;
      }
      handleMessage(msg);
    });
  }

  function send(msg) {
    if (state.ws && state.ws.readyState === WebSocket.OPEN) {
      state.ws.send(JSON.stringify(msg));
    }
  }

  function sendJoin() {
    const name = nameInput.value.trim();
    send({ type: "join", name });
  }

  function handleMessage(msg) {
    if (!msg || typeof msg.type !== "string") return;

    if (msg.type === "welcome") {
      state.localId = msg.id;
      state.world = msg.world || state.world;
      joinPanel.classList.add("hidden");
      state.joined = true;
    } else if (msg.type === "snapshot") {
      applySnapshot(msg.players || []);
      state.orbs = Array.isArray(msg.orbs) ? msg.orbs : state.orbs;
      state.powerups = Array.isArray(msg.powerups) ? msg.powerups : state.powerups;
      state.controlZone = msg.controlZone || state.controlZone;
      state.round = msg.round || state.round;
      applyEvents(msg.events || []);
      updateRoundHud();
    } else if (msg.type === "chat") {
      appendChat(msg.from || "server", msg.message || "");
    } else if (msg.type === "chat_history") {
      chatLog.replaceChildren();
      for (const item of msg.messages || []) {
        appendChat(item.from || "server", item.message || "");
      }
    } else if (msg.type === "player_joined") {
      appendSystem(`${msg.name || "Player"} joined`);
    } else if (msg.type === "player_left") {
      state.players.delete(msg.id);
      state.renderPlayers.delete(msg.id);
      appendSystem("Player left");
    } else if (msg.type === "pong") {
      if (typeof msg.t === "number") {
        pingEl.textContent = String(Math.max(0, Date.now() - msg.t));
      }
    } else if (msg.type === "error") {
      appendSystem(msg.message || "Server rejected a message");
    }
  }

  function applySnapshot(players) {
    const seen = new Set();
    for (const p of players) {
      if (!p || typeof p.id !== "string") continue;
      seen.add(p.id);
      state.players.set(p.id, p);
      if (!state.renderPlayers.has(p.id)) {
        state.renderPlayers.set(p.id, { ...p });
      }
    }
    for (const id of state.players.keys()) {
      if (!seen.has(id)) {
        state.players.delete(id);
        state.renderPlayers.delete(id);
      }
    }
    playersEl.textContent = String(state.players.size);
    const local = state.players.get(state.localId);
    scoreEl.textContent = String(local?.score || 0);
    boostEl.textContent = local?.boostMs > 0 ? `${Math.ceil(local.boostMs / 1000)}s` : "--";
    renderLeaderboard();
  }

  function renderLeaderboard() {
    const sorted = Array.from(state.players.values())
      .sort((a, b) => (b.score || 0) - (a.score || 0) || String(a.name).localeCompare(String(b.name)))
      .slice(0, 6);
    leaderboardEl.replaceChildren();
    for (const p of sorted) {
      const li = document.createElement("li");
      if (p.id === state.localId) li.className = "me";
      const name = document.createElement("span");
      name.textContent = `${p.name || "Player"} `;
      const score = document.createElement("b");
      score.textContent = String(p.score || 0);
      li.append(name, score);
      leaderboardEl.append(li);
    }
  }

  function formatTime(totalSeconds) {
    const safe = Math.max(0, Number(totalSeconds) || 0);
    const minutes = Math.floor(safe / 60);
    const seconds = Math.floor(safe % 60);
    return `${minutes}:${String(seconds).padStart(2, "0")}`;
  }

  function updateRoundHud() {
    roundTimeEl.textContent = formatTime(state.round.secondsRemaining);
    if (state.round.phase === "intermission") {
      const winner = state.round.lastWinner || {};
      roundBanner.innerHTML = `Round ${state.round.number} complete: <b>${escapeHtml(winner.name || "No winner")}</b> won with <b>${winner.score || 0}</b>. Next round in ${formatTime(state.round.secondsRemaining)}.`;
      roundBanner.classList.remove("hidden");
    } else {
      roundBanner.classList.add("hidden");
      roundBanner.textContent = "";
    }
  }

  function applyEvents(events) {
    let changed = false;
    for (const event of events) {
      if (!event || typeof event.id !== "number" || state.seenEvents.has(event.id)) continue;
      state.seenEvents.add(event.id);
      state.events.unshift(event);
      changed = true;
    }
    state.events = state.events.slice(0, 8);
    if (changed) renderEvents();
  }

  function renderEvents() {
    eventFeed.replaceChildren();
    for (const event of state.events.slice(0, 6)) {
      const line = document.createElement("div");
      line.className = "event-line";
      const label = document.createElement("strong");
      label.textContent = event.type || "event";
      const text = document.createElement("span");
      text.textContent = ` ${event.text || ""}`;
      line.append(label, text);
      eventFeed.append(line);
    }
  }

  function escapeHtml(value) {
    return String(value).replace(/[&<>"']/g, (c) => ({
      "&": "&amp;",
      "<": "&lt;",
      ">": "&gt;",
      "\"": "&quot;",
      "'": "&#39;"
    })[c]);
  }

  function appendChat(from, message) {
    const line = document.createElement("div");
    line.className = "chat-line";
    const name = document.createElement("b");
    name.textContent = from;
    const text = document.createElement("span");
    text.textContent = `: ${message}`;
    line.append(name, text);
    chatLog.append(line);
    chatLog.scrollTop = chatLog.scrollHeight;
  }

  function appendSystem(message) {
    const line = document.createElement("div");
    line.className = "chat-line";
    line.textContent = message;
    chatLog.append(line);
    chatLog.scrollTop = chatLog.scrollHeight;
  }

  function sendInput(force = false) {
    const key = JSON.stringify(state.keys);
    if (!force && key === state.lastInputSent) return;
    state.lastInputSent = key;
    send({ type: "input", ...state.keys, seq: ++state.seq });
  }

  function keyMap(key, value) {
    const k = key.toLowerCase();
    if (k === "w" || key === "ArrowUp") state.keys.up = value;
    else if (k === "s" || key === "ArrowDown") state.keys.down = value;
    else if (k === "a" || key === "ArrowLeft") state.keys.left = value;
    else if (k === "d" || key === "ArrowRight") state.keys.right = value;
    else return false;
    return true;
  }

  window.addEventListener("keydown", (event) => {
    if (event.target === chatInput || event.target === nameInput) {
      if (event.key === "Escape") event.target.blur();
      return;
    }
    if (event.key === "Enter") {
      chatInput.focus();
      return;
    }
    if (keyMap(event.key, true)) {
      event.preventDefault();
      sendInput();
    }
  });

  window.addEventListener("keyup", (event) => {
    if (keyMap(event.key, false)) {
      event.preventDefault();
      sendInput();
    }
  });

  chatInput.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      chatInput.blur();
    } else if (event.key === "Enter") {
      const message = chatInput.value.trim();
      if (message) {
        send({ type: "chat", message });
        chatInput.value = "";
      }
    }
  });

  joinBtn.addEventListener("click", () => {
    state.joined = true;
    sendJoin();
  });

  nameInput.addEventListener("keydown", (event) => {
    if (event.key === "Enter") joinBtn.click();
  });

  function updateRenderPlayers() {
    for (const [id, target] of state.players) {
      const rp = state.renderPlayers.get(id) || { ...target };
      rp.x += (target.x - rp.x) * 0.28;
      rp.y += (target.y - rp.y) * 0.28;
      rp.name = target.name;
      rp.color = target.color;
      rp.score = target.score || 0;
      rp.boostMs = target.boostMs || 0;
      state.renderPlayers.set(id, rp);
    }
  }

  function drawGrid(viewW, viewH) {
    const spacing = 80;
    const left = state.camera.x - viewW / 2;
    const top = state.camera.y - viewH / 2;
    ctx.strokeStyle = "rgba(160,180,210,0.08)";
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let x = Math.floor(left / spacing) * spacing; x < left + viewW; x += spacing) {
      ctx.moveTo(Math.round(x - left), 0);
      ctx.lineTo(Math.round(x - left), viewH);
    }
    for (let y = Math.floor(top / spacing) * spacing; y < top + viewH; y += spacing) {
      ctx.moveTo(0, Math.round(y - top));
      ctx.lineTo(viewW, Math.round(y - top));
    }
    ctx.stroke();
  }

  function drawMinimap(viewW, viewH) {
    const mapW = Math.min(220, viewW * 0.22);
    const mapH = mapW * (state.world.height / state.world.width);
    const x = viewW - mapW - 18;
    const y = 18;
    const sx = mapW / state.world.width;
    const sy = mapH / state.world.height;

    ctx.save();
    ctx.globalAlpha = 0.92;
    ctx.fillStyle = "rgba(13,17,23,0.82)";
    ctx.strokeStyle = "rgba(160,180,210,0.24)";
    ctx.lineWidth = 1;
    ctx.fillRect(x, y, mapW, mapH);
    ctx.strokeRect(x, y, mapW, mapH);

    ctx.fillStyle = "rgba(255,204,102,0.28)";
    for (const o of state.world.obstacles || []) {
      ctx.fillRect(x + o.x * sx, y + o.y * sy, o.w * sx, o.h * sy);
    }

    ctx.fillStyle = "rgba(122,245,155,0.32)";
    const zone = state.controlZone;
    if (zone) {
      ctx.beginPath();
      ctx.arc(x + zone.x * sx, y + zone.y * sy, zone.radius * sx, 0, Math.PI * 2);
      ctx.fill();
    }

    for (const orb of state.orbs) {
      ctx.fillStyle = orb.color || "#66ccff";
      ctx.fillRect(x + orb.x * sx - 1.5, y + orb.y * sy - 1.5, 3, 3);
    }

    for (const powerup of state.powerups) {
      ctx.fillStyle = powerup.color || "#c9a7ff";
      ctx.fillRect(x + powerup.x * sx - 2, y + powerup.y * sy - 2, 4, 4);
    }

    for (const p of state.players.values()) {
      ctx.fillStyle = p.id === state.localId ? "#ffffff" : p.color || "#66ccff";
      ctx.beginPath();
      ctx.arc(x + p.x * sx, y + p.y * sy, p.id === state.localId ? 4 : 3, 0, Math.PI * 2);
      ctx.fill();
    }

    ctx.restore();
  }

  function render() {
    resize();
    updateRenderPlayers();

    const viewW = canvas.clientWidth;
    const viewH = canvas.clientHeight;
    const local = state.renderPlayers.get(state.localId);
    if (local) {
      state.camera.x += (local.x - state.camera.x) * 0.12;
      state.camera.y += (local.y - state.camera.y) * 0.12;
    }

    const left = state.camera.x - viewW / 2;
    const top = state.camera.y - viewH / 2;

    ctx.clearRect(0, 0, viewW, viewH);
    ctx.fillStyle = "#111822";
    ctx.fillRect(0, 0, viewW, viewH);
    drawGrid(viewW, viewH);

    ctx.save();
    ctx.translate(-left, -top);

    ctx.strokeStyle = "rgba(237,242,247,0.36)";
    ctx.lineWidth = 4;
    ctx.strokeRect(0, 0, state.world.width, state.world.height);

    const zone = state.controlZone;
    if (zone) {
      const pulse = 0.5 + Math.sin((performance.now() - state.startedAt) / 520) * 0.12;
      ctx.beginPath();
      ctx.fillStyle = `rgba(122,245,155,${0.12 + pulse * 0.04})`;
      ctx.strokeStyle = "rgba(122,245,155,0.58)";
      ctx.lineWidth = 3;
      ctx.arc(zone.x, zone.y, zone.radius, 0, Math.PI * 2);
      ctx.fill();
      ctx.stroke();
      ctx.font = "14px system-ui, sans-serif";
      ctx.textAlign = "center";
      ctx.fillStyle = "#7af59b";
      ctx.fillText(`Control zone +${zone.pointsPerSecond || 2}/s`, zone.x, zone.y + 5);
    }

    ctx.fillStyle = "rgba(255,204,102,0.24)";
    ctx.strokeStyle = "rgba(255,204,102,0.58)";
    ctx.lineWidth = 2;
    for (const o of state.world.obstacles || []) {
      ctx.fillRect(o.x, o.y, o.w, o.h);
      ctx.strokeRect(o.x, o.y, o.w, o.h);
    }

    for (const orb of state.orbs) {
      const t = (performance.now() - state.startedAt) / 450 + Number(String(orb.id || "0").replace(/\D/g, "")) * 0.3;
      const radius = 10 + Math.sin(t) * 2 + (orb.value > 5 ? 3 : 0);
      ctx.beginPath();
      ctx.shadowColor = orb.color || "#66ccff";
      ctx.shadowBlur = orb.value > 5 ? 18 : 10;
      ctx.fillStyle = orb.color || "#66ccff";
      ctx.arc(orb.x, orb.y, radius, 0, Math.PI * 2);
      ctx.fill();
      ctx.shadowBlur = 0;
      ctx.lineWidth = 2;
      ctx.strokeStyle = "rgba(255,255,255,0.62)";
      ctx.stroke();
      if (orb.value > 5) {
        ctx.font = "11px system-ui, sans-serif";
        ctx.textAlign = "center";
        ctx.fillStyle = "#0d1117";
        ctx.fillText(String(orb.value), orb.x, orb.y + 4);
      }
    }

    for (const powerup of state.powerups) {
      const t = (performance.now() - state.startedAt) / 380;
      const size = 15 + Math.sin(t) * 2;
      ctx.save();
      ctx.translate(powerup.x, powerup.y);
      ctx.rotate(t * 0.8);
      ctx.beginPath();
      ctx.shadowColor = powerup.color || "#c9a7ff";
      ctx.shadowBlur = 18;
      ctx.fillStyle = powerup.color || "#c9a7ff";
      ctx.moveTo(0, -size);
      ctx.lineTo(size, 0);
      ctx.lineTo(0, size);
      ctx.lineTo(-size, 0);
      ctx.closePath();
      ctx.fill();
      ctx.shadowBlur = 0;
      ctx.strokeStyle = "rgba(255,255,255,0.70)";
      ctx.lineWidth = 2;
      ctx.stroke();
      ctx.restore();
    }

    for (const p of state.renderPlayers.values()) {
      ctx.beginPath();
      ctx.fillStyle = p.color || "#66ccff";
      ctx.shadowColor = p.color || "#66ccff";
      ctx.shadowBlur = p.id === state.localId ? 18 : 8;
      ctx.arc(p.x, p.y, 18, 0, Math.PI * 2);
      ctx.fill();
      ctx.shadowBlur = 0;
      ctx.lineWidth = p.id === state.localId ? 3 : 2;
      ctx.strokeStyle = p.id === state.localId ? "#ffffff" : "rgba(255,255,255,0.58)";
      ctx.stroke();
      if (p.boostMs > 0) {
        ctx.beginPath();
        ctx.strokeStyle = "rgba(201,167,255,0.78)";
        ctx.lineWidth = 3;
        ctx.arc(p.x, p.y, 25, 0, Math.PI * 2);
        ctx.stroke();
      }

      ctx.font = "13px system-ui, sans-serif";
      ctx.textAlign = "center";
      ctx.fillStyle = "#edf2f7";
      ctx.fillText(p.name || "Player", p.x, p.y - 28);
      ctx.fillStyle = "#ffcc66";
      ctx.fillText(String(p.score || 0), p.x, p.y + 39);
    }
    ctx.restore();
    drawMinimap(viewW, viewH);

    requestAnimationFrame(render);
  }

  setInterval(() => sendInput(true), 100);
  setInterval(() => send({ type: "ping", t: Date.now() }), 2000);

  window.addEventListener("resize", resize);
  resize();
  connect();
  requestAnimationFrame(render);
})();
