(() => {
  const canvas = document.getElementById("arena");
  const ctx = canvas.getContext("2d");
  const arenaWrap = document.querySelector(".arena-wrap");
  const statusEl = document.getElementById("status");
  const playersEl = document.getElementById("players");
  const scoreEl = document.getElementById("score");
  const questEl = document.getElementById("quest");
  const roundTimeEl = document.getElementById("roundTime");
  const boostEl = document.getElementById("boost");
  const pingEl = document.getElementById("ping");
  const fpsEl = document.getElementById("fps");
  const snapshotRateEl = document.getElementById("snapshotRate");
  const effectsBtn = document.getElementById("effectsBtn");
  const statsLink = document.getElementById("statsLink");
  const settingsBtn = document.getElementById("settingsBtn");
  const settingsPanel = document.getElementById("settingsPanel");
  const settingsCloseBtn = document.getElementById("settingsCloseBtn");
  const settingsSoundBtn = document.getElementById("settingsSoundBtn");
  const settingsEffectsBtn = document.getElementById("settingsEffectsBtn");
  const settingsCopyRoomBtn = document.getElementById("settingsCopyRoomBtn");
  const settingsStatsLink = document.getElementById("settingsStatsLink");
  const settingsRoomLabel = document.getElementById("settingsRoomLabel");
  const joinPanel = document.getElementById("joinPanel");
  const nameInput = document.getElementById("nameInput");
  const roomInput = document.getElementById("roomInput");
  const copyRoomBtn = document.getElementById("copyRoomBtn");
  const newRoomBtn = document.getElementById("newRoomBtn");
  const refreshRoomsBtn = document.getElementById("refreshRoomsBtn");
  const roomList = document.getElementById("roomList");
  const lobbyLeaderboardMeta = document.getElementById("lobbyLeaderboardMeta");
  const lobbyLeaderboard = document.getElementById("lobbyLeaderboard");
  const joinBtn = document.getElementById("joinBtn");
  const watchBtn = document.getElementById("watchBtn");
  const spectatorBar = document.getElementById("spectatorBar");
  const spectatorJoinBtn = document.getElementById("spectatorJoinBtn");
  const joinError = document.getElementById("joinError");
  const chatLog = document.getElementById("chatLog");
  const chatInput = document.getElementById("chatInput");
  const leaderboardEl = document.getElementById("leaderboard");
  const roundBanner = document.getElementById("roundBanner");
  const roundSummary = document.getElementById("roundSummary");
  const roundSummaryTitle = document.getElementById("roundSummaryTitle");
  const roundSummaryList = document.getElementById("roundSummaryList");
  const roundSummaryNext = document.getElementById("roundSummaryNext");
  const roundSummaryClose = document.getElementById("roundSummaryClose");
  const eventFeed = document.getElementById("eventFeed");
  const touchStick = document.getElementById("touchStick");
  const touchKnob = document.getElementById("touchKnob");
  const mobileChatBtn = document.getElementById("mobileChatBtn");
  const mobileInfoBtn = document.getElementById("mobileInfoBtn");
  const objectiveLabel = document.getElementById("objectiveLabel");
  const objectiveDistance = document.getElementById("objectiveDistance");
  const mobileChatBadge = document.getElementById("mobileChatBadge");
  const dashBtn = document.getElementById("dashBtn");
  const shieldBtn = document.getElementById("shieldBtn");
  const magnetBtn = document.getElementById("magnetBtn");
  const soundBtn = document.getElementById("soundBtn");
  const dashState = document.getElementById("dashState");
  const shieldState = document.getElementById("shieldState");
  const magnetState = document.getElementById("magnetState");

  const state = {
    ws: null,
    joined: false,
    pendingJoin: false,
    spectator: false,
    pendingSpectate: false,
    protocolVersion: 2,
    localId: null,
    room: "public",
    world: { width: 2000, height: 1200, obstacles: [] },
    orbs: [],
    powerups: [],
    hazards: [],
    controlZone: { x: 1000, y: 600, radius: 150, pointsPerSecond: 2 },
    round: { number: 1, phase: "active", secondsRemaining: 180, lastWinner: { name: "No winner yet", score: 0 } },
    events: [],
    seenEvents: new Set(),
    players: new Map(),
    renderPlayers: new Map(),
    keys: { up: false, down: false, left: false, right: false },
    seq: 0,
    lastInputSent: "",
    touchPointerId: null,
    touchCapture: null,
    chatUnread: 0,
    lastLocalScore: null,
    floaters: [],
    particles: [],
    trails: [],
    soundEnabled: localStorage.getItem("vix.sound") === "on",
    reducedEffects: localStorage.getItem("vix.effects") === "reduced" || window.matchMedia("(prefers-reduced-motion: reduce)").matches,
    audio: null,
    activeObjective: null,
    camera: { x: 1000, y: 600 },
    layoutHeight: 0,
    canvasPixelWidth: 0,
    canvasPixelHeight: 0,
    pingTimer: 0,
    reconnectTimer: 0,
    roomDirectory: null,
    leaderboardPreviewTimer: 0,
    leaderboardPreviewRequestId: 0,
    dismissedRoundSummaryKey: "",
    perf: { frames: 0, fps: 0, lastFpsAt: performance.now(), snapshots: 0, snapshotRate: 0, lastSnapshotRateAt: performance.now() },
    startedAt: performance.now()
  };

  function haptic(pattern = 12) {
    if (!window.matchMedia("(pointer: coarse)").matches || !("vibrate" in navigator)) return;
    try {
      navigator.vibrate(pattern);
    } catch {
      // Haptics are optional and unsupported on several browsers.
    }
  }

  function resize() {
    const rect = canvas.getBoundingClientRect();
    const dpr = Math.max(1, Math.min(2, window.devicePixelRatio || 1));
    const width = Math.max(1, Math.floor(rect.width * dpr));
    const height = Math.max(1, Math.floor(rect.height * dpr));
    if (width !== state.canvasPixelWidth || height !== state.canvasPixelHeight) {
      canvas.width = width;
      canvas.height = height;
      state.canvasPixelWidth = width;
      state.canvasPixelHeight = height;
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }

  function setAppHeight(force = false) {
    const visualHeight = Math.round(window.visualViewport?.height || window.innerHeight || document.documentElement.clientHeight);
    const fallbackHeight = Math.round(window.innerHeight || visualHeight);
    const inputFocused = document.activeElement === chatInput || document.activeElement === nameInput || document.activeElement === roomInput;
    const keyboardOpen = inputFocused && visualHeight < fallbackHeight * 0.86;
    const nextHeight = Math.max(320, keyboardOpen ? visualHeight : fallbackHeight);

    if (force || state.layoutHeight !== nextHeight) {
      state.layoutHeight = nextHeight;
      document.documentElement.style.setProperty("--app-height", `${state.layoutHeight}px`);
    }

    document.body.classList.toggle("chat-focused", document.activeElement === chatInput);
    document.body.classList.toggle("keyboard-open", keyboardOpen);
    window.scrollTo(0, 0);
  }

  function settleViewport() {
    window.scrollTo(0, 0);
    requestAnimationFrame(() => {
      setAppHeight();
      window.scrollTo(0, 0);
    });
    setTimeout(() => {
      setAppHeight();
      window.scrollTo(0, 0);
    }, 220);
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
      if (state.joined || state.pendingJoin) sendJoin();
    });

    ws.addEventListener("close", (event) => {
      setStatus("offline", false);
      if (event.reason) appendSystem(`Disconnected: ${event.reason}`);
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

  function resumeKey(room) {
    return `vix.resume.${room}`;
  }

  function sendJoin() {
    const name = nameInput.value.trim();
    const room = sanitizeRoom(roomInput?.value || roomFromLocation() || "public");
    if (name) localStorage.setItem("vix.name", name);
    localStorage.setItem("vix.room", room);
    state.room = room;
    const spectate = state.pendingSpectate || (state.spectator && !state.pendingJoin);
    const message = { type: "join", name, room, protocolVersion: state.protocolVersion, supports: ["snapshot_delta"] };
    if (spectate) {
      message.spectate = true;
    } else {
      const resume = localStorage.getItem(resumeKey(room));
      if (resume) message.resume = resume;
    }
    send(message);
  }

  const savedName = localStorage.getItem("vix.name");
  if (savedName) nameInput.value = savedName.slice(0, 18);
  const savedRoom = roomFromLocation() || localStorage.getItem("vix.room") || "public";
  if (roomInput) roomInput.value = sanitizeRoom(savedRoom);

  // Mirrors the server's sanitizeRoomCode: runs of non-alphanumerics collapse
  // to a single dash, so client links always match the server-side room.
  function sanitizeRoom(value) {
    const clean = String(value || "public")
      .toLowerCase()
      .replace(/[^a-z0-9]+/g, "-")
      .replace(/^-+|-+$/g, "")
      .slice(0, 24)
      .replace(/-+$/g, "");
    return clean.length >= 3 ? clean : "public";
  }

  function roomFromLocation() {
    const params = new URLSearchParams(window.location.search);
    return params.get("room") || window.location.hash.replace(/^#room=/, "");
  }

  function roomLink() {
    const url = new URL(window.location.href);
    url.searchParams.set("room", sanitizeRoom(roomInput?.value || state.room));
    url.hash = "";
    return url.toString();
  }

  function updateStatsLink() {
    if (!statsLink) return;
    const room = sanitizeRoom(roomInput?.value || state.room || "public");
    const href = room === "public" ? "/stats" : `/stats?room=${encodeURIComponent(room)}`;
    statsLink.href = href;
    if (settingsStatsLink) settingsStatsLink.href = href;
    if (settingsRoomLabel) settingsRoomLabel.textContent = `Room ${room}`;
  }

  function selectedRoom() {
    return sanitizeRoom(roomInput?.value || state.room || "public");
  }

  function roomMetricLabel(room) {
    const humans = Number(room?.humans || 0);
    const bots = Number(room?.bots || 0);
    const players = Number(room?.players || humans + bots);
    const parts = [`${players} online`];
    if (humans > 0) parts.push(`${humans} human${humans === 1 ? "" : "s"}`);
    if (bots > 0) parts.push(`${bots} bot${bots === 1 ? "" : "s"}`);
    return parts.join(" · ");
  }

  function renderRoomDirectory(data = state.roomDirectory) {
    if (!roomList) return;
    const currentRoom = selectedRoom();
    const rooms = Array.isArray(data?.rooms) ? data.rooms : [];
    const listedRooms = rooms.filter((room) => room && room.listed && typeof room.code === "string");
    const privateSummary = rooms.find((room) => room && !room.listed && Number(room.roomCount || 0) > 0);

    roomList.replaceChildren();
    const publicRoom = listedRooms.find((room) => room.code === "public") || { code: "public", players: 0, humans: 0, bots: 0 };
    const orderedRooms = [publicRoom, ...listedRooms.filter((room) => room.code !== "public")];

    for (const room of orderedRooms) {
      const code = sanitizeRoom(room.code);
      const button = document.createElement("button");
      button.className = `room-card${code === currentRoom ? " selected" : ""}`;
      button.type = "button";
      button.dataset.roomCode = code;
      const name = document.createElement("span");
      name.textContent = code === "public" ? "Public arena" : code;
      const metric = document.createElement("strong");
      metric.textContent = roomMetricLabel(room);
      button.append(name, metric);
      button.addEventListener("click", (event) => {
        event.stopPropagation();
        selectRoomCode(code);
      });
      roomList.append(button);
    }

    if (privateSummary) {
      const row = document.createElement("div");
      row.className = "room-card private-summary";
      const name = document.createElement("span");
      const count = Number(privateSummary.roomCount || 0);
      name.textContent = `${count} private ${count === 1 ? "room" : "rooms"}`;
      const metric = document.createElement("strong");
      metric.textContent = roomMetricLabel(privateSummary);
      row.append(name, metric);
      roomList.append(row);
    }
  }

  function selectRoomCode(code) {
    if (!roomInput) return;
    roomInput.value = sanitizeRoom(code);
    updateStatsLink();
    renderRoomDirectory();
    scheduleLeaderboardPreview();
    haptic(6);
  }

  async function refreshRooms() {
    if (!roomList || state.joined) return;
    try {
      const response = await fetch("/api/rooms", { cache: "no-store" });
      if (!response.ok) throw new Error("rooms unavailable");
      state.roomDirectory = await response.json();
      renderRoomDirectory();
    } catch {
      state.roomDirectory = null;
      renderRoomDirectory();
    }
  }

  function generateRoomCode() {
    const bytes = new Uint8Array(3);
    crypto.getRandomValues(bytes);
    return `arena-${Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("")}`;
  }

  function leaderboardUrlForRoom(room) {
    return `/api/leaderboard?room=${encodeURIComponent(sanitizeRoom(room || "public"))}`;
  }

  function renderLeaderboardPreview(data = null) {
    if (!lobbyLeaderboard || !lobbyLeaderboardMeta) return;
    const room = selectedRoom();
    lobbyLeaderboardMeta.textContent = room;
    lobbyLeaderboard.replaceChildren();

    const entries = Array.isArray(data?.entries) ? data.entries.slice(0, 3) : [];
    if (!entries.length) {
      const empty = document.createElement("li");
      empty.textContent = "No completed rounds yet";
      lobbyLeaderboard.append(empty);
      return;
    }

    for (const entry of entries) {
      const row = document.createElement("li");
      const name = document.createElement("strong");
      name.textContent = `#${entry.rank || "?"} ${entry.name || "Player"}`;
      const stats = document.createElement("span");
      stats.textContent = `${entry.wins || 0}W · best ${entry.bestScore || 0}`;
      row.append(name, stats);
      lobbyLeaderboard.append(row);
    }
  }

  async function refreshLeaderboardPreview() {
    if (!lobbyLeaderboard || state.joined) return;
    const requestId = ++state.leaderboardPreviewRequestId;
    const room = selectedRoom();
    renderLeaderboardPreview(null);
    try {
      const response = await fetch(leaderboardUrlForRoom(room), { cache: "no-store" });
      if (!response.ok) throw new Error("leaderboard unavailable");
      const data = await response.json();
      if (requestId === state.leaderboardPreviewRequestId) {
        renderLeaderboardPreview(data);
      }
    } catch {
      if (requestId === state.leaderboardPreviewRequestId) {
        renderLeaderboardPreview(null);
      }
    }
  }

  function scheduleLeaderboardPreview() {
    clearTimeout(state.leaderboardPreviewTimer);
    renderLeaderboardPreview(null);
    state.leaderboardPreviewTimer = setTimeout(refreshLeaderboardPreview, 220);
  }

  function handleMessage(msg) {
    if (!msg || typeof msg.type !== "string") return;

    if (msg.type === "welcome") {
      const spectator = Boolean(msg.spectator);
      state.spectator = spectator;
      state.localId = spectator ? null : msg.id;
      state.room = msg.room || state.room;
      if (roomInput) roomInput.value = state.room;
      updateStatsLink();
      if (typeof msg.protocolVersion === "number") state.protocolVersion = msg.protocolVersion;
      state.world = msg.world || state.world;
      joinPanel.classList.add("hidden");
      state.joined = true;
      state.pendingJoin = false;
      state.pendingSpectate = false;
      document.body.classList.toggle("spectating", spectator);
      if (spectatorBar) spectatorBar.classList.toggle("hidden", !spectator);
      setJoinError("");
      if (spectator) {
        resetTouchInput();
        appendSystem(`Spectating ${state.room} - watching live`);
      } else if (typeof msg.resumeToken === "string" && msg.resumeToken) {
        localStorage.setItem(resumeKey(state.room), msg.resumeToken);
      }
      if (msg.resumed) appendSystem("Reconnected - progress restored");
    } else if (msg.type === "snapshot") {
      recordSnapshotMessage();
      rememberSnapshotMeta(msg);
      applySnapshot(msg.players || []);
      state.orbs = Array.isArray(msg.orbs) ? msg.orbs : state.orbs;
      state.powerups = Array.isArray(msg.powerups) ? msg.powerups : state.powerups;
      state.hazards = Array.isArray(msg.hazards) ? msg.hazards : state.hazards;
      state.controlZone = msg.controlZone || state.controlZone;
      state.round = msg.round || state.round;
      applyEvents(msg.events || []);
      updateRoundHud();
    } else if (msg.type === "snapshot_delta") {
      recordSnapshotMessage();
      applySnapshotDelta(msg);
    } else if (msg.type === "chat") {
      appendChat(msg.from || "server", msg.message || "");
    } else if (msg.type === "chat_history") {
      chatLog.replaceChildren();
      for (const item of msg.messages || []) {
        appendChat(item.from || "server", item.message || "", false);
      }
    } else if (msg.type === "player_joined") {
      appendSystem(`${msg.name || "Player"} joined`);
    } else if (msg.type === "player_left") {
      state.players.delete(msg.id);
      state.renderPlayers.delete(msg.id);
      appendSystem("Player left");
    } else if (msg.type === "ability") {
      const p = state.renderPlayers.get(msg.id) || state.players.get(msg.id);
      if (p) {
        burstParticles(p.x, p.y, msg.ability === "dash" ? "#66ccff" : msg.ability === "shield" ? "#7af59b" : "#c9a7ff", 18);
      }
      if (msg.id === state.localId) playTone(msg.ability === "dash" ? 440 : msg.ability === "shield" ? 330 : 520, 0.08, "triangle");
    } else if (msg.type === "pong") {
      if (typeof msg.t === "number") {
        pingEl.textContent = String(Math.max(0, Date.now() - msg.t));
      }
    } else if (msg.type === "server_shutdown") {
      const text = msg.message || "Server is restarting - reconnecting shortly";
      appendSystem(text);
      roundBanner.textContent = text;
      roundBanner.classList.remove("hidden");
      setStatus("restarting", false);
    } else if (msg.type === "error") {
      appendSystem(msg.message || "Server rejected a message");
      if (!state.joined && state.pendingJoin) {
        state.pendingJoin = false;
        setJoinError(msg.message || "Join failed");
      }
    }
  }

  function setJoinError(message) {
    if (!joinError) return;
    joinError.textContent = message;
    joinError.classList.toggle("hidden", !message);
  }

  function rememberSnapshotMeta(msg) {
    if (typeof msg.snapshotId === "number") state.lastSnapshotId = msg.snapshotId;
    if (typeof msg.tick === "number") state.lastServerTick = msg.tick;
    if (typeof msg.serverTimeMs === "number") state.lastServerTimeMs = msg.serverTimeMs;
  }

  function recordSnapshotMessage() {
    state.perf.snapshots += 1;
  }

  function updatePerformanceHud(now) {
    state.perf.frames += 1;
    if (now - state.perf.lastFpsAt >= 1000) {
      state.perf.fps = Math.round((state.perf.frames * 1000) / Math.max(1, now - state.perf.lastFpsAt));
      state.perf.frames = 0;
      state.perf.lastFpsAt = now;
      fpsEl.textContent = String(state.perf.fps);
    }
    if (now - state.perf.lastSnapshotRateAt >= 1000) {
      state.perf.snapshotRate = Math.round((state.perf.snapshots * 1000) / Math.max(1, now - state.perf.lastSnapshotRateAt));
      state.perf.snapshots = 0;
      state.perf.lastSnapshotRateAt = now;
      snapshotRateEl.textContent = String(state.perf.snapshotRate);
    }
  }

  function updateEffectsButton() {
    effectsBtn.textContent = state.reducedEffects ? "FX low" : "FX on";
    effectsBtn.setAttribute("aria-pressed", String(!state.reducedEffects));
    settingsEffectsBtn.textContent = state.reducedEffects ? "FX low" : "FX on";
    settingsEffectsBtn.setAttribute("aria-pressed", String(!state.reducedEffects));
  }

  function updateSoundButtons() {
    soundBtn.textContent = state.soundEnabled ? "Sound on" : "Sound off";
    soundBtn.classList.toggle("ready", state.soundEnabled);
    settingsSoundBtn.textContent = state.soundEnabled ? "Sound on" : "Sound off";
    settingsSoundBtn.setAttribute("aria-pressed", String(state.soundEnabled));
  }

  function setSettingsOpen(open) {
    settingsPanel.classList.toggle("hidden", !open);
    settingsBtn.setAttribute("aria-expanded", String(open));
    if (open) {
      document.body.classList.add("show-panels");
      updateStatsLink();
      updateSoundButtons();
      updateEffectsButton();
    }
  }

  async function copyCurrentRoomLink() {
    haptic(8);
    try {
      await navigator.clipboard.writeText(roomLink());
      appendSystem("Room link copied");
    } catch {
      appendSystem(roomLink());
    }
  }

  function mergeById(current, upserts, removedIds) {
    const byId = new Map();
    for (const item of current || []) {
      if (item && typeof item.id === "string") byId.set(item.id, item);
    }
    for (const id of removedIds || []) {
      byId.delete(id);
    }
    for (const item of upserts || []) {
      if (item && typeof item.id === "string") byId.set(item.id, item);
    }
    return Array.from(byId.values());
  }

  function requestResync() {
    const now = performance.now();
    if (state.lastResyncRequestAt && now - state.lastResyncRequestAt < 1500) return;
    state.lastResyncRequestAt = now;
    send({ type: "resync" });
    appendSystem("Snapshot resync requested");
  }

  function applySnapshotDelta(msg) {
    if (typeof msg.baseSnapshotId === "number" && state.lastSnapshotId && msg.baseSnapshotId !== state.lastSnapshotId) {
      requestResync();
      return;
    }

    rememberSnapshotMeta(msg);
    applySnapshot(msg.players || [], msg.removedPlayers || []);
    state.orbs = mergeById(state.orbs, msg.orbs || [], msg.removedOrbs || []);
    state.powerups = mergeById(state.powerups, msg.powerups || [], msg.removedPowerups || []);
    state.hazards = mergeById(state.hazards, msg.hazards || [], msg.removedHazards || []);
    state.controlZone = msg.controlZone || state.controlZone;
    state.round = msg.round || state.round;
    applyEvents(msg.events || []);
    updateRoundHud();
  }

  function applySnapshot(players, removedPlayers = null) {
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
      if (removedPlayers ? removedPlayers.includes(id) : !seen.has(id)) {
        state.players.delete(id);
        state.renderPlayers.delete(id);
      }
    }
    playersEl.textContent = String(state.players.size);
    const local = state.players.get(state.localId);
    if (local && state.lastLocalScore !== null && (local.score || 0) > state.lastLocalScore) {
      state.floaters.push({
        x: local.x,
        y: local.y - 30,
        text: `+${(local.score || 0) - state.lastLocalScore}`,
        color: local.color || "#ffcc66",
        createdAt: performance.now()
      });
      state.floaters = state.floaters.slice(-8);
      burstParticles(local.x, local.y, local.color || "#ffcc66", 14);
      playTone(660, 0.055, "sine");
    } else if (local && state.lastLocalScore !== null && (local.score || 0) < state.lastLocalScore) {
      state.floaters.push({
        x: local.x,
        y: local.y - 30,
        text: `-${state.lastLocalScore - (local.score || 0)}`,
        color: "#ff5c8a",
        createdAt: performance.now()
      });
      state.floaters = state.floaters.slice(-8);
      burstParticles(local.x, local.y, "#ff5c8a", 10);
      playTone(180, 0.06, "sawtooth");
    }
    if (local) state.lastLocalScore = local.score || 0;
    else state.lastLocalScore = null;
    scoreEl.textContent = String(local?.score || 0);
    questEl.textContent = local?.quest ? `${local.quest.progress || 0}/${local.quest.goal || 3}` : "0/3";
    boostEl.textContent = local?.boostMs > 0 ? `${Math.ceil(local.boostMs / 1000)}s` : "--";
    updateAbilityHud(local);
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
      name.textContent = `${p.name || "Player"}${p.bot ? " [BOT]" : ""} `;
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

  function formatCooldown(ms) {
    const safe = Math.max(0, Number(ms) || 0);
    return safe <= 0 ? "Ready" : `${Math.ceil(safe / 1000)}s`;
  }

  function setAbilityButton(button, label, activeMs, cooldownMs) {
    const active = Number(activeMs) > 0;
    const cooling = Number(cooldownMs) > 0;
    button.classList.toggle("active", active);
    button.classList.toggle("cooldown", !active && cooling);
    button.classList.toggle("ready", !active && !cooling);
    label.textContent = active ? `${Math.ceil(activeMs / 1000)}s` : formatCooldown(cooldownMs);
  }

  function updateAbilityHud(local) {
    const abilities = local?.abilities || {};
    setAbilityButton(dashBtn, dashState, 0, abilities.dashCooldownMs || 0);
    setAbilityButton(shieldBtn, shieldState, abilities.shieldMs || 0, abilities.shieldCooldownMs || 0);
    setAbilityButton(magnetBtn, magnetState, abilities.magnetMs || 0, abilities.magnetCooldownMs || 0);
    updateSoundButtons();
  }

  function ensureAudio() {
    if (!state.audio) {
      state.audio = new (window.AudioContext || window.webkitAudioContext)();
    }
    if (state.audio.state === "suspended") state.audio.resume();
  }

  function playTone(freq, duration = 0.07, type = "sine") {
    if (!state.soundEnabled) return;
    try {
      ensureAudio();
      const osc = state.audio.createOscillator();
      const gain = state.audio.createGain();
      osc.type = type;
      osc.frequency.value = freq;
      gain.gain.setValueAtTime(0.0001, state.audio.currentTime);
      gain.gain.exponentialRampToValueAtTime(0.035, state.audio.currentTime + 0.01);
      gain.gain.exponentialRampToValueAtTime(0.0001, state.audio.currentTime + duration);
      osc.connect(gain);
      gain.connect(state.audio.destination);
      osc.start();
      osc.stop(state.audio.currentTime + duration + 0.02);
    } catch {
      state.soundEnabled = false;
    }
  }

  function burstParticles(x, y, color, count) {
    if (state.reducedEffects) return;
    const now = performance.now();
    for (let i = 0; i < count; i += 1) {
      const angle = (Math.PI * 2 * i) / count + Math.random() * 0.4;
      const speed = 55 + Math.random() * 95;
      state.particles.push({
        x,
        y,
        vx: Math.cos(angle) * speed,
        vy: Math.sin(angle) * speed,
        color,
        createdAt: now,
        ttl: 520 + Math.random() * 280
      });
    }
    state.particles = state.particles.slice(-180);
  }

  function clamp(value, min, max) {
    return Math.max(min, Math.min(max, value));
  }

  function distance(a, b) {
    return Math.hypot((a?.x || 0) - (b?.x || 0), (a?.y || 0) - (b?.y || 0));
  }

  function describeObjective(local) {
    if (state.spectator) {
      const leader = leaderRenderPlayer();
      return {
        label: "Spectating",
        detail: leader ? `Leader ${leader.name || "?"} · ${leader.score || 0}` : "Watching live",
        x: state.camera.x,
        y: state.camera.y,
        color: "#c9a7ff"
      };
    }
    if (!state.joined || !local) {
      return { label: "Join arena", detail: "Pick a name to enter", x: state.camera.x, y: state.camera.y, color: "#66ccff" };
    }

    if (state.round.phase === "intermission") {
      return {
        label: "Round reset",
        detail: `Next round in ${formatTime(state.round.secondsRemaining)}`,
        x: local.x,
        y: local.y,
        color: "#ffcc66"
      };
    }

    const candidates = [];
    for (const orb of state.orbs) {
      if (!Number.isFinite(orb.x) || !Number.isFinite(orb.y)) continue;
      candidates.push({
        label: `${orb.value || 5}-point orb`,
        detail: "Collect",
        x: orb.x,
        y: orb.y,
        color: orb.color || "#66ccff",
        dist: distance(local, orb)
      });
    }

    for (const powerup of state.powerups) {
      if (!Number.isFinite(powerup.x) || !Number.isFinite(powerup.y)) continue;
      candidates.push({
        label: "Speed boost",
        detail: "Grab boost",
        x: powerup.x,
        y: powerup.y,
        color: powerup.color || "#c9a7ff",
        dist: distance(local, powerup) * 0.9
      });
    }

    for (const hazard of state.hazards) {
      if (!Number.isFinite(hazard.x) || !Number.isFinite(hazard.y)) continue;
      const rawDist = distance(local, hazard);
      const hazardRadius = Number(hazard.radius) || 46;
      if (rawDist <= hazardRadius + 90) {
        candidates.push({
          label: "Exit danger zone",
          detail: `-${hazard.penaltyPerSecond || 3}/s`,
          x: hazard.x,
          y: hazard.y,
          color: hazard.color || "#ff5c8a",
          dist: 0
        });
      }
    }

    const zone = state.controlZone;
    if (zone && Number.isFinite(zone.x) && Number.isFinite(zone.y)) {
      const rawDist = distance(local, zone);
      candidates.push({
        label: rawDist <= (zone.radius || 0) ? "Hold control zone" : "Reach control zone",
        detail: `+${zone.pointsPerSecond || 2}/s`,
        x: zone.x,
        y: zone.y,
        color: "#7af59b",
        dist: Math.max(0, rawDist - (zone.radius || 0)) * 0.75
      });
    }

    if (!candidates.length) {
      return { label: "Explore arena", detail: "Waiting for spawns", x: local.x, y: local.y, color: "#66ccff" };
    }

    candidates.sort((a, b) => a.dist - b.dist);
    const best = candidates[0];
    best.realDist = Math.round(distance(local, best));
    return best;
  }

  function updateObjective(local) {
    const objective = describeObjective(local);
    state.activeObjective = objective;
    objectiveLabel.textContent = objective.label;
    objectiveDistance.textContent = objective.realDist ? `${objective.detail} - ${objective.realDist}u` : objective.detail;
  }

  function roundSummaryKey() {
    return `${state.room}:${state.round.number}:${state.round.lastWinner?.id || state.round.lastWinner?.name || "none"}`;
  }

  function hideRoundSummary() {
    if (!roundSummary) return;
    roundSummary.classList.add("hidden");
  }

  function renderRoundSummary() {
    if (!roundSummary || !roundSummaryTitle || !roundSummaryList || !roundSummaryNext) return;
    if (!state.joined || state.round.phase !== "intermission") {
      hideRoundSummary();
      return;
    }

    const key = roundSummaryKey();
    if (state.dismissedRoundSummaryKey === key) {
      hideRoundSummary();
      return;
    }

    const winner = state.round.lastWinner || {};
    roundSummaryTitle.textContent = `${winner.name || "No winner"} won with ${winner.score || 0}`;
    roundSummaryNext.textContent = `Next round in ${formatTime(state.round.secondsRemaining)}`;
    roundSummaryList.replaceChildren();

    const standings = Array.from(state.players.values())
      .sort((a, b) => (b.score || 0) - (a.score || 0) || String(a.name || "").localeCompare(String(b.name || "")))
      .slice(0, 5);

    if (!standings.length) {
      const row = document.createElement("li");
      row.textContent = "No players scored this round";
      roundSummaryList.append(row);
    } else {
      standings.forEach((player, index) => {
        const row = document.createElement("li");
        const rank = document.createElement("b");
        rank.textContent = `#${index + 1}`;
        const name = document.createElement("span");
        name.textContent = `${player.name || "Player"}${player.bot ? " [BOT]" : ""}`;
        const score = document.createElement("strong");
        score.textContent = `${player.score || 0}`;
        row.append(rank, name, score);
        roundSummaryList.append(row);
      });
    }

    roundSummary.classList.remove("hidden");
  }

  function updateRoundHud() {
    roundTimeEl.textContent = formatTime(state.round.secondsRemaining);
    if (state.round.phase === "intermission") {
      const winner = state.round.lastWinner || {};
      roundBanner.replaceChildren(
        document.createTextNode(`Round ${state.round.number} complete: `),
        strongText(winner.name || "No winner"),
        document.createTextNode(" won with "),
        strongText(winner.score || 0),
        document.createTextNode(`. Next round in ${formatTime(state.round.secondsRemaining)}.`)
      );
      roundBanner.classList.remove("hidden");
    } else {
      roundBanner.classList.add("hidden");
      roundBanner.textContent = "";
      hideRoundSummary();
    }
    renderRoundSummary();
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

  function strongText(value) {
    const el = document.createElement("b");
    el.textContent = String(value);
    return el;
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

  function isMobileLayout() {
    return window.matchMedia("(max-width: 720px), (max-width: 980px) and (max-height: 560px) and (orientation: landscape)").matches;
  }

  function updateChatBadge() {
    mobileChatBadge.textContent = String(Math.min(9, state.chatUnread));
    mobileChatBadge.classList.toggle("hidden", state.chatUnread < 1);
  }

  function clearChatUnread() {
    state.chatUnread = 0;
    updateChatBadge();
  }

  const MUTE_KEY = "vix.muted";
  const mutedNames = loadMutedNames();
  const CHAT_LOG_LIMIT = 150;

  function loadMutedNames() {
    try {
      const raw = JSON.parse(localStorage.getItem(MUTE_KEY) || "[]");
      return new Set(Array.isArray(raw) ? raw.filter((n) => typeof n === "string") : []);
    } catch {
      return new Set();
    }
  }

  function saveMutedNames() {
    try {
      localStorage.setItem(MUTE_KEY, JSON.stringify([...mutedNames]));
    } catch {
      // localStorage may be unavailable (private mode); muting stays in-memory.
    }
  }

  function isMuted(name) {
    return mutedNames.has(String(name).toLowerCase());
  }

  function toggleMute(name) {
    if (!name) return;
    const key = String(name).toLowerCase();
    if (mutedNames.has(key)) {
      mutedNames.delete(key);
      appendSystem(`Unmuted ${name}`);
    } else {
      mutedNames.add(key);
      appendSystem(`Muted ${name} - click their name to unmute`);
    }
    saveMutedNames();
  }

  function trimChatLog() {
    while (chatLog.childElementCount > CHAT_LOG_LIMIT) {
      chatLog.removeChild(chatLog.firstElementChild);
    }
  }

  function appendChat(from, message, notify = true) {
    if (isMuted(from)) return;
    const line = document.createElement("div");
    line.className = "chat-line";
    const name = document.createElement("b");
    name.textContent = from;
    name.className = "chat-name";
    name.dataset.chatFrom = from;
    name.title = "Click to mute/unmute";
    const text = document.createElement("span");
    text.textContent = `: ${message}`;
    line.append(name, text);
    chatLog.append(line);
    trimChatLog();
    chatLog.scrollTop = chatLog.scrollHeight;
    if (notify && isMobileLayout() && !document.body.classList.contains("show-chat")) {
      state.chatUnread += 1;
      updateChatBadge();
    }
  }

  function appendSystem(message) {
    const line = document.createElement("div");
    line.className = "chat-line";
    line.textContent = message;
    chatLog.append(line);
    trimChatLog();
    chatLog.scrollTop = chatLog.scrollHeight;
  }

  function sendInput(force = false) {
    if (state.spectator) return;
    const key = JSON.stringify(state.keys);
    if (!force && key === state.lastInputSent) return;
    state.lastInputSent = key;
    send({ type: "input", ...state.keys, seq: ++state.seq });
  }

  function castAbility(ability) {
    if (!state.joined || state.spectator) return;
    haptic(10);
    send({ type: "ability", ability });
  }

  function sendQuickPing(message) {
    if (!state.joined || state.spectator) return;
    haptic(8);
    send({ type: "chat", message });
  }

  function setTouchInput(dx, dy) {
    const dead = 0.22;
    state.keys.left = dx < -dead;
    state.keys.right = dx > dead;
    state.keys.up = dy < -dead;
    state.keys.down = dy > dead;
    sendInput();
  }

  function resetTouchInput() {
    if (state.touchCapture && state.touchPointerId !== null) {
      try {
        state.touchCapture.releasePointerCapture(state.touchPointerId);
      } catch {
        // Pointer capture may already be released by the browser.
      }
    }
    state.touchPointerId = null;
    state.touchCapture = null;
    touchStick.classList.remove("floating");
    touchStick.style.left = "";
    touchStick.style.top = "";
    touchStick.style.bottom = "";
    touchKnob.style.transform = "translate(-50%, -50%)";
    state.keys.left = false;
    state.keys.right = false;
    state.keys.up = false;
    state.keys.down = false;
    sendInput(true);
  }

  function positionFloatingStick(event) {
    const wrap = arenaWrap.getBoundingClientRect();
    const size = touchStick.offsetWidth || 128;
    const pad = 10;
    const left = clamp(event.clientX - wrap.left - size / 2, pad, Math.max(pad, wrap.width - size - pad));
    const top = clamp(event.clientY - wrap.top - size / 2, pad, Math.max(pad, wrap.height - size - pad));
    touchStick.classList.add("floating");
    touchStick.style.left = `${left}px`;
    touchStick.style.top = `${top}px`;
    touchStick.style.bottom = "auto";
  }

  function startTouchMove(event, floating) {
    if (state.touchPointerId !== null) return;
    event.preventDefault();
    if (floating) positionFloatingStick(event);
    haptic(6);
    state.touchPointerId = event.pointerId;
    state.touchCapture = floating ? arenaWrap : touchStick;
    try {
      state.touchCapture.setPointerCapture(event.pointerId);
    } catch {
      // Some mobile browsers expose pointer events without capture support.
    }
    updateTouchStick(event);
  }

  function updateTouchStick(event) {
    const rect = touchStick.getBoundingClientRect();
    const centerX = rect.left + rect.width / 2;
    const centerY = rect.top + rect.height / 2;
    const max = rect.width * 0.34;
    const rawX = event.clientX - centerX;
    const rawY = event.clientY - centerY;
    const dist = Math.hypot(rawX, rawY);
    const scale = dist > max ? max / dist : 1;
    const x = rawX * scale;
    const y = rawY * scale;
    touchKnob.style.transform = `translate(calc(-50% + ${x}px), calc(-50% + ${y}px))`;
    setTouchInput(x / max, y / max);
  }

  function canStartFloatingTouch(event) {
    if (!isMobileLayout() || event.pointerType === "mouse" || !state.joined) return false;
    const target = event.target instanceof Element ? event.target : null;
    if (!target || target.closest("input, button, .join-panel, .chat-panel, .mobile-actions, .leaderboard-panel, .events-panel, .touch-stick")) {
      return false;
    }
    return target === canvas || target === arenaWrap;
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
    if (event.target === chatInput || event.target === nameInput || event.target === roomInput) {
      if (event.key === "Escape") {
        event.preventDefault();
        event.target.blur();
        if (event.target === chatInput) {
          document.body.classList.remove("show-chat", "chat-focused");
          settleViewport();
        }
        setSettingsOpen(false);
      }
      return;
    }
    if (event.key === "Escape" && !settingsPanel.classList.contains("hidden")) {
      event.preventDefault();
      setSettingsOpen(false);
      return;
    }
    if (event.key === "Enter") {
      chatInput.focus();
      return;
    }
    if (event.code === "Space") {
      event.preventDefault();
      castAbility("dash");
      return;
    }
    if (event.key === "Shift") {
      event.preventDefault();
      castAbility("shield");
      return;
    }
    if (event.key.toLowerCase() === "e") {
      event.preventDefault();
      castAbility("magnet");
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

  chatLog.addEventListener("click", (event) => {
    const target = event.target instanceof Element ? event.target.closest("[data-chat-from]") : null;
    if (target) {
      haptic(6);
      toggleMute(target.dataset.chatFrom);
    }
  });

  chatInput.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      event.preventDefault();
      document.body.classList.remove("show-chat", "chat-focused");
      chatInput.blur();
      settleViewport();
    } else if (event.key === "Enter") {
      event.preventDefault();
      const message = chatInput.value.trim();
      if (message && !state.spectator) {
        send({ type: "chat", message });
        chatInput.value = "";
      }
      if (isMobileLayout()) {
        document.body.classList.remove("show-chat");
      }
      chatInput.blur();
      settleViewport();
    }
  });

  chatInput.addEventListener("focus", () => {
    clearChatUnread();
    setAppHeight();
  });

  chatInput.addEventListener("blur", () => {
    if (isMobileLayout()) {
      document.body.classList.remove("show-chat");
    }
    settleViewport();
  });

  nameInput.addEventListener("focus", setAppHeight);
  nameInput.addEventListener("blur", settleViewport);
  roomInput?.addEventListener("focus", setAppHeight);
  roomInput?.addEventListener("blur", () => {
    roomInput.value = sanitizeRoom(roomInput.value);
    updateStatsLink();
    renderRoomDirectory();
    scheduleLeaderboardPreview();
    settleViewport();
  });

  joinBtn.addEventListener("click", () => {
    haptic(10);
    setJoinError("");
    state.pendingJoin = true;
    state.pendingSpectate = false;
    sendJoin();
  });

  watchBtn?.addEventListener("click", () => {
    haptic(8);
    setJoinError("");
    state.pendingJoin = false;
    state.pendingSpectate = true;
    sendJoin();
  });

  spectatorJoinBtn?.addEventListener("click", () => {
    haptic(10);
    // Convert an active spectator into a real player with a plain join.
    state.spectator = false;
    state.pendingSpectate = false;
    state.pendingJoin = true;
    sendJoin();
  });

  nameInput.addEventListener("keydown", (event) => {
    if (event.key === "Enter") joinBtn.click();
  });
  roomInput?.addEventListener("keydown", (event) => {
    if (event.key === "Enter") joinBtn.click();
  });
  roomInput?.addEventListener("input", () => {
    roomInput.value = sanitizeRoom(roomInput.value);
    updateStatsLink();
    renderRoomDirectory();
    scheduleLeaderboardPreview();
  });
  copyRoomBtn?.addEventListener("click", async () => {
    roomInput.value = sanitizeRoom(roomInput.value);
    updateStatsLink();
    renderRoomDirectory();
    scheduleLeaderboardPreview();
    await copyCurrentRoomLink();
  });
  newRoomBtn?.addEventListener("click", () => {
    roomInput.value = generateRoomCode();
    updateStatsLink();
    renderRoomDirectory();
    scheduleLeaderboardPreview();
    haptic(8);
  });
  refreshRoomsBtn?.addEventListener("click", () => {
    haptic(6);
    refreshRooms();
  });
  roomList?.addEventListener("click", (event) => {
    const button = event.target instanceof Element ? event.target.closest("[data-room-code]") : null;
    if (!(button instanceof HTMLElement)) return;
    selectRoomCode(button.dataset.roomCode || "public");
  });

  dashBtn.addEventListener("click", () => castAbility("dash"));
  shieldBtn.addEventListener("click", () => castAbility("shield"));
  magnetBtn.addEventListener("click", () => castAbility("magnet"));
  soundBtn.addEventListener("click", () => {
    haptic(8);
    state.soundEnabled = !state.soundEnabled;
    localStorage.setItem("vix.sound", state.soundEnabled ? "on" : "off");
    if (state.soundEnabled) {
      ensureAudio();
      playTone(520, 0.08, "sine");
    }
    updateAbilityHud(state.players.get(state.localId));
  });
  settingsSoundBtn.addEventListener("click", () => soundBtn.click());
  effectsBtn.addEventListener("click", () => {
    haptic(6);
    state.reducedEffects = !state.reducedEffects;
    localStorage.setItem("vix.effects", state.reducedEffects ? "reduced" : "full");
    if (state.reducedEffects) {
      state.floaters = [];
      state.particles = [];
      state.trails = [];
    }
    updateEffectsButton();
  });
  settingsEffectsBtn.addEventListener("click", () => effectsBtn.click());
  settingsCopyRoomBtn.addEventListener("click", copyCurrentRoomLink);
  settingsBtn.addEventListener("click", () => {
    haptic(6);
    setSettingsOpen(settingsPanel.classList.contains("hidden"));
  });
  settingsCloseBtn.addEventListener("click", () => {
    haptic(6);
    setSettingsOpen(false);
  });
  roundSummaryClose?.addEventListener("click", () => {
    state.dismissedRoundSummaryKey = roundSummaryKey();
    hideRoundSummary();
    haptic(6);
  });
  document.querySelectorAll(".ping-button").forEach((button) => {
    button.addEventListener("click", () => sendQuickPing(button.dataset.ping || "Ping"));
  });

  touchStick.addEventListener("pointerdown", (event) => {
    startTouchMove(event, false);
  });

  arenaWrap.addEventListener("pointerdown", (event) => {
    if (canStartFloatingTouch(event)) startTouchMove(event, true);
  });

  window.addEventListener("pointermove", (event) => {
    if (event.pointerId !== state.touchPointerId) return;
    event.preventDefault();
    updateTouchStick(event);
  });

  window.addEventListener("pointerup", (event) => {
    if (event.pointerId !== state.touchPointerId) return;
    event.preventDefault();
    resetTouchInput();
  });

  window.addEventListener("pointercancel", (event) => {
    if (event.pointerId === state.touchPointerId) resetTouchInput();
  });

  mobileChatBtn.addEventListener("click", () => {
    haptic(8);
    document.body.classList.toggle("show-chat");
    if (document.body.classList.contains("show-chat")) {
      clearChatUnread();
      chatInput.focus();
    } else {
      chatInput.blur();
    }
    settleViewport();
  });

  mobileInfoBtn.addEventListener("click", () => {
    haptic(8);
    document.body.classList.toggle("show-panels");
    setSettingsOpen(settingsPanel.classList.contains("hidden"));
  });

  function updateRenderPlayers() {
    const now = performance.now();
    for (const [id, target] of state.players) {
      const rp = state.renderPlayers.get(id) || { ...target };
      const moved = Math.hypot((target.x || 0) - (rp.x || 0), (target.y || 0) - (rp.y || 0)) > 4;
      if (!state.reducedEffects && moved && Math.random() < 0.45) {
        state.trails.push({
          x: rp.x,
          y: rp.y,
          radius: id === state.localId ? 13 : 9,
          color: target.color || "#66ccff",
          createdAt: now,
          ttl: id === state.localId ? 520 : 360
        });
      }
      rp.x += (target.x - rp.x) * 0.28;
      rp.y += (target.y - rp.y) * 0.28;
      rp.name = target.name;
      rp.color = target.color;
      rp.bot = Boolean(target.bot);
      rp.score = target.score || 0;
      rp.boostMs = target.boostMs || 0;
      rp.abilities = target.abilities || {};
      state.renderPlayers.set(id, rp);
    }
    state.trails = state.trails.slice(-140);
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
    if (isMobileLayout() && document.body.classList.contains("chat-focused")) return;

    const mobile = isMobileLayout();
    const mapW = mobile ? Math.min(132, viewW * 0.30) : Math.min(220, viewW * 0.22);
    const mapH = mapW * (state.world.height / state.world.width);
    const x = viewW - mapW - 18;
    const y = mobile ? 64 : 18;
    if (y + mapH + 18 > viewH) return;
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

    ctx.fillStyle = "rgba(255,92,138,0.30)";
    for (const hazard of state.hazards) {
      const radius = Math.max(3, (hazard.radius || 46) * sx);
      ctx.beginPath();
      ctx.arc(x + hazard.x * sx, y + hazard.y * sy, radius, 0, Math.PI * 2);
      ctx.fill();
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

  function drawObjectiveMarker(viewW, viewH, left, top) {
    if (isMobileLayout() && document.body.classList.contains("chat-focused")) return;

    const objective = state.activeObjective;
    if (!objective || !objective.realDist) return;

    const screenX = objective.x - left;
    const screenY = objective.y - top;
    const margin = 32;
    const visible = screenX > margin && screenX < viewW - margin && screenY > margin && screenY < viewH - margin;
    if (visible) return;

    const markerX = clamp(screenX, margin, viewW - margin);
    const markerY = clamp(screenY, margin, viewH - margin);
    const angle = Math.atan2(screenY - viewH / 2, screenX - viewW / 2);

    ctx.save();
    ctx.translate(markerX, markerY);
    ctx.rotate(angle);
    ctx.fillStyle = objective.color || "#66ccff";
    ctx.shadowColor = objective.color || "#66ccff";
    ctx.shadowBlur = 14;
    ctx.beginPath();
    ctx.moveTo(13, 0);
    ctx.lineTo(-8, -8);
    ctx.lineTo(-4, 0);
    ctx.lineTo(-8, 8);
    ctx.closePath();
    ctx.fill();
    ctx.restore();

    ctx.save();
    ctx.font = "12px system-ui, sans-serif";
    ctx.textAlign = markerX > viewW * 0.5 ? "right" : "left";
    ctx.fillStyle = "#edf2f7";
    ctx.shadowColor = "rgba(13,17,23,0.95)";
    ctx.shadowBlur = 8;
    const labelX = markerX > viewW * 0.5 ? markerX - 18 : markerX + 18;
    ctx.fillText(`${objective.label} ${objective.realDist}u`, labelX, clamp(markerY + 4, 18, viewH - 18));
    ctx.restore();
  }

  function drawFloaters(now) {
    if (state.reducedEffects) {
      state.floaters = [];
      return;
    }
    state.floaters = state.floaters.filter((floater) => now - floater.createdAt < 1100);
    for (const floater of state.floaters) {
      const age = now - floater.createdAt;
      const progress = age / 1100;
      ctx.save();
      ctx.globalAlpha = 1 - progress;
      ctx.font = "700 18px system-ui, sans-serif";
      ctx.textAlign = "center";
      ctx.fillStyle = floater.color || "#ffcc66";
      ctx.shadowColor = "rgba(13,17,23,0.95)";
      ctx.shadowBlur = 8;
      ctx.fillText(floater.text, floater.x, floater.y - progress * 42);
      ctx.restore();
    }
  }

  function drawTrails(now) {
    if (state.reducedEffects) {
      state.trails = [];
      return;
    }
    state.trails = state.trails.filter((trail) => now - trail.createdAt < trail.ttl);
    for (const trail of state.trails) {
      const progress = (now - trail.createdAt) / trail.ttl;
      ctx.save();
      ctx.globalAlpha = (1 - progress) * 0.22;
      ctx.fillStyle = trail.color || "#66ccff";
      ctx.beginPath();
      ctx.arc(trail.x, trail.y, trail.radius * (1 + progress * 0.8), 0, Math.PI * 2);
      ctx.fill();
      ctx.restore();
    }
  }

  function drawParticles(now) {
    if (state.reducedEffects) {
      state.particles = [];
      return;
    }
    state.particles = state.particles.filter((particle) => now - particle.createdAt < particle.ttl);
    for (const particle of state.particles) {
      const age = now - particle.createdAt;
      const dt = age / 1000;
      const progress = age / particle.ttl;
      ctx.save();
      ctx.globalAlpha = 1 - progress;
      ctx.fillStyle = particle.color || "#66ccff";
      ctx.beginPath();
      ctx.arc(particle.x + particle.vx * dt, particle.y + particle.vy * dt, 3.5 * (1 - progress) + 1, 0, Math.PI * 2);
      ctx.fill();
      ctx.restore();
    }
  }

  function leaderRenderPlayer() {
    let best = null;
    for (const rp of state.renderPlayers.values()) {
      if (!best || (rp.score || 0) > (best.score || 0)) best = rp;
    }
    return best;
  }

  function render() {
    const now = performance.now();
    updatePerformanceHud(now);
    resize();
    updateRenderPlayers();

    const viewW = canvas.clientWidth;
    const viewH = canvas.clientHeight;
    const local = state.renderPlayers.get(state.localId);
    // Spectators have no local player, so the camera trails the current
    // leader (highest score) to keep the action in frame.
    const focus = local || (state.spectator ? leaderRenderPlayer() : null);
    if (focus) {
      state.camera.x += (focus.x - state.camera.x) * 0.12;
      state.camera.y += (focus.y - state.camera.y) * 0.12;
    }
    updateObjective(local);

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
      const pulse = 0.5 + Math.sin((now - state.startedAt) / 520) * 0.12;
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

    for (const hazard of state.hazards) {
      const radius = Number(hazard.radius) || 46;
      const pulse = state.reducedEffects ? 0 : Math.sin((now - state.startedAt) / 260 + radius) * 4;
      ctx.beginPath();
      ctx.fillStyle = "rgba(255,92,138,0.16)";
      ctx.strokeStyle = hazard.color || "#ff5c8a";
      ctx.lineWidth = 3;
      ctx.arc(hazard.x, hazard.y, radius + pulse, 0, Math.PI * 2);
      ctx.fill();
      ctx.stroke();
      ctx.beginPath();
      ctx.strokeStyle = "rgba(255,255,255,0.20)";
      ctx.setLineDash([8, 8]);
      ctx.arc(hazard.x, hazard.y, Math.max(8, radius - 13), 0, Math.PI * 2);
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.font = "700 12px system-ui, sans-serif";
      ctx.textAlign = "center";
      ctx.fillStyle = "#ffdbe5";
      ctx.fillText(`-${hazard.penaltyPerSecond || 3}/s`, hazard.x, hazard.y + 4);
    }

    for (const orb of state.orbs) {
      const t = (now - state.startedAt) / 450 + Number(String(orb.id || "0").replace(/\D/g, "")) * 0.3;
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
      const t = (now - state.startedAt) / 380;
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

    drawTrails(now);

    for (const p of state.renderPlayers.values()) {
      ctx.beginPath();
      ctx.fillStyle = p.color || "#66ccff";
      ctx.shadowColor = p.color || "#66ccff";
      ctx.shadowBlur = p.id === state.localId ? 18 : 8;
      ctx.arc(p.x, p.y, 18, 0, Math.PI * 2);
      ctx.fill();
      ctx.shadowBlur = 0;
      ctx.lineWidth = p.id === state.localId ? 3 : 2;
      ctx.strokeStyle = p.id === state.localId ? "#ffffff" : p.bot ? "rgba(154,168,184,0.70)" : "rgba(255,255,255,0.58)";
      ctx.stroke();
      if (p.bot) {
        ctx.font = "700 10px system-ui, sans-serif";
        ctx.textAlign = "center";
        ctx.fillStyle = "#0d1117";
        ctx.fillText("AI", p.x, p.y + 3);
      }
      if (p.boostMs > 0) {
        ctx.beginPath();
        ctx.strokeStyle = "rgba(201,167,255,0.78)";
        ctx.lineWidth = 3;
        ctx.arc(p.x, p.y, 25, 0, Math.PI * 2);
        ctx.stroke();
      }
      if (p.abilities?.shieldMs > 0) {
        ctx.beginPath();
        ctx.strokeStyle = "rgba(122,245,155,0.70)";
        ctx.lineWidth = 4;
        ctx.arc(p.x, p.y, 31, 0, Math.PI * 2);
        ctx.stroke();
      }
      if (p.abilities?.magnetMs > 0) {
        ctx.beginPath();
        ctx.strokeStyle = "rgba(201,167,255,0.34)";
        ctx.lineWidth = 2;
        ctx.arc(p.x, p.y, 74 + Math.sin(now / 180) * 4, 0, Math.PI * 2);
        ctx.stroke();
      }

      ctx.font = "13px system-ui, sans-serif";
      ctx.textAlign = "center";
      ctx.fillStyle = p.bot ? "#9aa8b8" : "#edf2f7";
      ctx.fillText(`${p.name || "Player"}${p.bot ? " BOT" : ""}`, p.x, p.y - 28);
      ctx.fillStyle = "#ffcc66";
      ctx.fillText(String(p.score || 0), p.x, p.y + 39);
    }
    drawFloaters(now);
    drawParticles(now);
    ctx.restore();
    drawObjectiveMarker(viewW, viewH, left, top);
    drawMinimap(viewW, viewH);

    requestAnimationFrame(render);
  }

  setInterval(() => sendInput(true), 100);
  setInterval(() => send({ type: "ping", t: Date.now() }), 2000);
  setInterval(refreshRooms, 10000);

  window.addEventListener("resize", () => {
    setAppHeight();
    resize();
  });
  window.visualViewport?.addEventListener("resize", setAppHeight);
  window.visualViewport?.addEventListener("scroll", settleViewport);
  setAppHeight(true);
  resize();
  connect();
  updateStatsLink();
  updateEffectsButton();
  updateSoundButtons();
  setSettingsOpen(false);
  renderRoomDirectory();
  refreshRooms();
  renderLeaderboardPreview();
  refreshLeaderboardPreview();
  if ("serviceWorker" in navigator) {
    window.addEventListener("load", () => {
      navigator.serviceWorker.register("/sw.js").catch(() => {
        // The realtime app works normally without offline asset caching.
      });
    });
  }
  requestAnimationFrame(render);
})();
