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
  const joinPanel = document.getElementById("joinPanel");
  const nameInput = document.getElementById("nameInput");
  const joinBtn = document.getElementById("joinBtn");
  const chatLog = document.getElementById("chatLog");
  const chatInput = document.getElementById("chatInput");
  const leaderboardEl = document.getElementById("leaderboard");
  const roundBanner = document.getElementById("roundBanner");
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
    touchPointerId: null,
    touchCapture: null,
    chatUnread: 0,
    lastLocalScore: null,
    floaters: [],
    particles: [],
    trails: [],
    soundEnabled: localStorage.getItem("vix.sound") === "on",
    audio: null,
    activeObjective: null,
    camera: { x: 1000, y: 600 },
    layoutHeight: 0,
    canvasPixelWidth: 0,
    canvasPixelHeight: 0,
    pingTimer: 0,
    reconnectTimer: 0,
    startedAt: performance.now()
  };

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
    const nextHeight = Math.max(320, fallbackHeight);
    const inputFocused = document.activeElement === chatInput || document.activeElement === nameInput;
    const previousHeight = state.layoutHeight || nextHeight;
    const keyboardOpen = inputFocused && (visualHeight < previousHeight * 0.82 || nextHeight < previousHeight * 0.82);

    if (force || !state.layoutHeight || !keyboardOpen || nextHeight > state.layoutHeight) {
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
    if (name) localStorage.setItem("vix.name", name);
    send({ type: "join", name });
  }

  const savedName = localStorage.getItem("vix.name");
  if (savedName) nameInput.value = savedName.slice(0, 18);

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
    soundBtn.textContent = state.soundEnabled ? "Sound on" : "Sound off";
    soundBtn.classList.toggle("ready", state.soundEnabled);
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

  function isMobileLayout() {
    return window.matchMedia("(max-width: 720px)").matches;
  }

  function updateChatBadge() {
    mobileChatBadge.textContent = String(Math.min(9, state.chatUnread));
    mobileChatBadge.classList.toggle("hidden", state.chatUnread < 1);
  }

  function clearChatUnread() {
    state.chatUnread = 0;
    updateChatBadge();
  }

  function appendChat(from, message, notify = true) {
    const line = document.createElement("div");
    line.className = "chat-line";
    const name = document.createElement("b");
    name.textContent = from;
    const text = document.createElement("span");
    text.textContent = `: ${message}`;
    line.append(name, text);
    chatLog.append(line);
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
    chatLog.scrollTop = chatLog.scrollHeight;
  }

  function sendInput(force = false) {
    const key = JSON.stringify(state.keys);
    if (!force && key === state.lastInputSent) return;
    state.lastInputSent = key;
    send({ type: "input", ...state.keys, seq: ++state.seq });
  }

  function castAbility(ability) {
    if (!state.joined) return;
    send({ type: "ability", ability });
  }

  function sendQuickPing(message) {
    if (!state.joined) return;
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
    if (event.target === chatInput || event.target === nameInput) {
      if (event.key === "Escape") event.target.blur();
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

  chatInput.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      chatInput.blur();
    } else if (event.key === "Enter") {
      event.preventDefault();
      const message = chatInput.value.trim();
      if (message) {
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

  joinBtn.addEventListener("click", () => {
    state.joined = true;
    sendJoin();
  });

  nameInput.addEventListener("keydown", (event) => {
    if (event.key === "Enter") joinBtn.click();
  });

  dashBtn.addEventListener("click", () => castAbility("dash"));
  shieldBtn.addEventListener("click", () => castAbility("shield"));
  magnetBtn.addEventListener("click", () => castAbility("magnet"));
  soundBtn.addEventListener("click", () => {
    state.soundEnabled = !state.soundEnabled;
    localStorage.setItem("vix.sound", state.soundEnabled ? "on" : "off");
    if (state.soundEnabled) {
      ensureAudio();
      playTone(520, 0.08, "sine");
    }
    updateAbilityHud(state.players.get(state.localId));
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
    document.body.classList.toggle("show-panels");
  });

  function updateRenderPlayers() {
    const now = performance.now();
    for (const [id, target] of state.players) {
      const rp = state.renderPlayers.get(id) || { ...target };
      const moved = Math.hypot((target.x || 0) - (rp.x || 0), (target.y || 0) - (rp.y || 0)) > 4;
      if (moved && Math.random() < 0.45) {
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

  function render() {
    const now = performance.now();
    resize();
    updateRenderPlayers();

    const viewW = canvas.clientWidth;
    const viewH = canvas.clientHeight;
    const local = state.renderPlayers.get(state.localId);
    if (local) {
      state.camera.x += (local.x - state.camera.x) * 0.12;
      state.camera.y += (local.y - state.camera.y) * 0.12;
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

  window.addEventListener("resize", () => {
    setAppHeight();
    resize();
  });
  window.visualViewport?.addEventListener("resize", setAppHeight);
  window.visualViewport?.addEventListener("scroll", settleViewport);
  setAppHeight(true);
  resize();
  connect();
  requestAnimationFrame(render);
})();
