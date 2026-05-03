#include "GameServer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>

#include "Protocol.hpp"
#include "Utils.hpp"
#include "Validation.hpp"

namespace arena
{
  namespace
  {
    bool jsonBool(const nlohmann::json &j, const char *key)
    {
      return j.contains(key) && j.at(key).is_boolean() && j.at(key).get<bool>();
    }

    template <typename Clock>
    std::int64_t remainingMs(std::chrono::time_point<Clock> until, std::chrono::time_point<Clock> now)
    {
      return std::max<std::int64_t>(
          0,
          std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count());
    }
  }

  GameServer::GameServer()
      : rng_(std::random_device{}()),
        startedAt_(std::chrono::steady_clock::now()),
        roundStartedAt_(startedAt_),
        intermissionUntil_(startedAt_)
  {
    ensureOrbsLocked();
    ensurePowerupsLocked();
  }

  GameServer::~GameServer()
  {
    stop();
  }

  void GameServer::start()
  {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
    {
      return;
    }
    tickThread_ = std::thread(&GameServer::tickLoop, this);
  }

  void GameServer::stop()
  {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false))
    {
      return;
    }
    if (tickThread_.joinable())
    {
      tickThread_.join();
    }
  }

  void GameServer::onOpen(const std::shared_ptr<ClientConnection> &)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++totalConnections_;
  }

  void GameServer::onClose(ClientConnection *session)
  {
    nlohmann::json left;
    bool hasLeft = false;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = sessionToPlayer_.find(session);
      if (it != sessionToPlayer_.end())
      {
        const std::string id = it->second;
        sessionToPlayer_.erase(it);
        players_.erase(id);
        left = {{"type", "player_left"}, {"id", id}};
        hasLeft = true;
      }
    }

    if (hasLeft)
    {
      broadcast(left);
    }
  }

  void GameServer::onMessage(ClientConnection *session, const std::string &payload)
  {
    if (payload.size() > maxWsPayloadBytes)
    {
      send(session, errorMessage("message too large"));
      return;
    }

    auto message = nlohmann::json::parse(payload, nullptr, false);
    if (message.is_discarded() || !message.is_object())
    {
      send(session, errorMessage("invalid JSON object"));
      return;
    }

    if (!message.contains("type") || !message["type"].is_string())
    {
      send(session, errorMessage("missing message type"));
      return;
    }

    const std::string type = message["type"].get<std::string>();
    if (type == "join")
    {
      handleJoin(session, message);
    }
    else if (type == "input")
    {
      handleInput(session, message);
    }
    else if (type == "chat")
    {
      handleChat(session, message);
    }
    else if (type == "ping")
    {
      handlePing(session, message);
    }
    else if (type == "ability")
    {
      handleAbility(session, message);
    }
    else
    {
      send(session, errorMessage("unknown message type"));
    }
  }

  void GameServer::handleJoin(ClientConnection *session, const nlohmann::json &message)
  {
    std::string requested;
    if (message.contains("name") && message["name"].is_string())
    {
      requested = message["name"].get<std::string>();
    }

    std::vector<nlohmann::json> history;
    nlohmann::json welcome;
    nlohmann::json joined;
    nlohmann::json snapshot;
    bool newPlayer = false;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto existing = sessionToPlayer_.find(session);
      if (existing != sessionToPlayer_.end())
      {
        const auto pit = players_.find(existing->second);
        if (pit != players_.end())
        {
          welcome = {
              {"type", "welcome"},
              {"id", pit->second.id},
              {"world", worldSummary(world_)}};
          snapshot = snapshotLocked();
          history.assign(chatHistory_.begin(), chatHistory_.end());
        }
      }
      else
      {
        if (players_.size() >= maxPlayers_)
        {
          welcome = errorMessage("arena full");
          history.clear();
          snapshot.clear();
          newPlayer = false;
        }
        else
        {
          const std::uint64_t n = nextPlayerNumber_++;
          std::string name = sanitizeDisplayName(requested);
          if (name.empty())
          {
            name = makeGuestName(n);
          }

          auto [x, y] = world_.randomSpawn(rng_);
          Player player;
          player.id = makePlayerId(n);
          player.name = name;
          player.color = randomColor();
          player.x = x;
          player.y = y;
          player.session = session->shared_from_this();
          player.lastSeen = std::chrono::steady_clock::now();
          player.lastInput = player.lastSeen - std::chrono::seconds(1);
          player.lastChat = player.lastSeen - std::chrono::seconds(10);
          player.speedBoostUntil = player.lastSeen;
          player.dashReadyAt = player.lastSeen;
          player.shieldUntil = player.lastSeen;
          player.shieldReadyAt = player.lastSeen;
          player.magnetUntil = player.lastSeen;
          player.magnetReadyAt = player.lastSeen;

          const std::string id = player.id;
          const std::string playerName = player.name;
          sessionToPlayer_[session] = id;
          players_[id] = std::move(player);
          addEventLocked("join", playerName + " joined the arena");

          welcome = {{"type", "welcome"}, {"id", id}, {"world", worldSummary(world_)}};
          joined = {{"type", "player_joined"}, {"id", id}, {"name", playerName}};
          snapshot = snapshotLocked();
          history.assign(chatHistory_.begin(), chatHistory_.end());
          newPlayer = true;
        }
      }
    }

    if (!welcome.empty())
    {
      send(session, welcome);
      if (!history.empty())
      {
        send(session, {{"type", "chat_history"}, {"messages", history}});
      }
      if (!snapshot.empty())
      {
        send(session, snapshot);
      }
    }

    if (newPlayer)
    {
      broadcast(joined);
    }
  }

  void GameServer::handleInput(ClientConnection *session, const nlohmann::json &message)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto sit = sessionToPlayer_.find(session);
    if (sit == sessionToPlayer_.end())
    {
      return;
    }

    auto pit = players_.find(sit->second);
    if (pit == players_.end())
    {
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - pit->second.lastInput < std::chrono::milliseconds(35))
    {
      pit->second.lastSeen = now;
      return;
    }
    pit->second.lastInput = now;
    pit->second.input.up = jsonBool(message, "up");
    pit->second.input.down = jsonBool(message, "down");
    pit->second.input.left = jsonBool(message, "left");
    pit->second.input.right = jsonBool(message, "right");
    if (message.contains("seq") && message["seq"].is_number_unsigned())
    {
      pit->second.input.seq = message["seq"].get<std::uint64_t>();
    }
    pit->second.lastSeen = now;
  }

  void GameServer::handleChat(ClientConnection *session, const nlohmann::json &message)
  {
    if (!message.contains("message") || !message["message"].is_string())
    {
      send(session, errorMessage("chat message must be a string"));
      return;
    }

    const std::string text = sanitizeChatMessage(message["message"].get<std::string>());
    if (text.empty())
    {
      return;
    }

    nlohmann::json outgoing;
    bool accepted = false;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto sit = sessionToPlayer_.find(session);
      if (sit == sessionToPlayer_.end())
      {
        return;
      }
      auto pit = players_.find(sit->second);
      if (pit == players_.end())
      {
        return;
      }

      const auto now = std::chrono::steady_clock::now();
      if (now - pit->second.lastChat < std::chrono::milliseconds(800))
      {
        send(session, errorMessage("chat rate limit"));
        return;
      }

      pit->second.lastChat = now;
      pit->second.lastSeen = now;
      outgoing = {
          {"type", "chat"},
          {"from", pit->second.name},
          {"message", text},
          {"timestamp", isoTimestampUtc()}};
      chatHistory_.push_back(outgoing);
      while (chatHistory_.size() > 50)
      {
        chatHistory_.pop_front();
      }
      ++totalChatMessages_;
      accepted = true;
    }

    if (accepted)
    {
      broadcast(outgoing);
    }
  }

  void GameServer::handlePing(ClientConnection *session, const nlohmann::json &message)
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto sit = sessionToPlayer_.find(session);
      if (sit != sessionToPlayer_.end())
      {
        auto pit = players_.find(sit->second);
        if (pit != players_.end())
        {
          pit->second.lastSeen = std::chrono::steady_clock::now();
        }
      }
    }

    nlohmann::json pong = {{"type", "pong"}};
    if (message.contains("t"))
    {
      pong["t"] = message["t"];
    }
    send(session, pong);
  }

  void GameServer::handleAbility(ClientConnection *session, const nlohmann::json &message)
  {
    if (!message.contains("ability") || !message["ability"].is_string())
    {
      send(session, errorMessage("ability must be a string"));
      return;
    }

    const std::string ability = message["ability"].get<std::string>();
    nlohmann::json outgoing;
    bool accepted = false;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto sit = sessionToPlayer_.find(session);
      if (sit == sessionToPlayer_.end())
      {
        return;
      }
      auto pit = players_.find(sit->second);
      if (pit == players_.end())
      {
        return;
      }

      Player &player = pit->second;
      const auto now = std::chrono::steady_clock::now();
      player.lastSeen = now;

      if (ability == "dash")
      {
        if (now < player.dashReadyAt)
        {
          send(session, errorMessage("dash cooldown"));
          return;
        }
        player.dashReadyAt = now + std::chrono::milliseconds(dashCooldownMs_);
        applyDashLocked(player, now);
        addEventLocked("dash", player.name + " dashed");
        outgoing = {{"type", "ability"}, {"id", player.id}, {"ability", "dash"}, {"timestamp", isoTimestampUtc()}};
        accepted = true;
      }
      else if (ability == "shield")
      {
        if (now < player.shieldReadyAt)
        {
          send(session, errorMessage("shield cooldown"));
          return;
        }
        player.shieldUntil = now + std::chrono::milliseconds(shieldDurationMs_);
        player.shieldReadyAt = now + std::chrono::milliseconds(shieldCooldownMs_);
        addEventLocked("shield", player.name + " phased through obstacles");
        outgoing = {{"type", "ability"}, {"id", player.id}, {"ability", "shield"}, {"timestamp", isoTimestampUtc()}};
        accepted = true;
      }
      else if (ability == "magnet")
      {
        if (now < player.magnetReadyAt)
        {
          send(session, errorMessage("magnet cooldown"));
          return;
        }
        player.magnetUntil = now + std::chrono::milliseconds(magnetDurationMs_);
        player.magnetReadyAt = now + std::chrono::milliseconds(magnetCooldownMs_);
        addEventLocked("magnet", player.name + " activated magnet");
        outgoing = {{"type", "ability"}, {"id", player.id}, {"ability", "magnet"}, {"timestamp", isoTimestampUtc()}};
        accepted = true;
      }
      else
      {
        send(session, errorMessage("unknown ability"));
        return;
      }
    }

    if (accepted)
    {
      broadcast(outgoing);
    }
  }

  void GameServer::tickLoop()
  {
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    const auto interval = std::chrono::milliseconds(1000 / tickRateTarget_);

    while (running_.load())
    {
      const auto now = clock::now();
      const std::chrono::duration<double> elapsed = now - last;
      last = now;

      nlohmann::json snapshot;
      std::vector<nlohmann::json> leftEvents;
      std::vector<SessionPtr> sessions;

      {
        std::lock_guard<std::mutex> lock(mutex_);
        step(std::min(elapsed.count(), 0.2));
        cleanupStaleLocked(leftEvents);
        snapshot = snapshotLocked();
        sessions = liveSessionsLocked();
      }

      broadcastTo(sessions, snapshot);
      for (const auto &event : leftEvents)
      {
        broadcast(event);
      }

      std::this_thread::sleep_until(now + interval);
    }
  }

  void GameServer::step(double dt)
  {
    ensureOrbsLocked();
    ensurePowerupsLocked();
    updateRoundLocked();
    const auto now = std::chrono::steady_clock::now();

    for (auto &[_, player] : players_)
    {
      double dx = 0.0;
      double dy = 0.0;

      if (player.input.left)
        dx -= 1.0;
      if (player.input.right)
        dx += 1.0;
      if (player.input.up)
        dy -= 1.0;
      if (player.input.down)
        dy += 1.0;

      const double len = std::sqrt(dx * dx + dy * dy);
      if (len > 0.0)
      {
        dx /= len;
        dy /= len;
        player.facingX = dx;
        player.facingY = dy;
      }

      const double speed = playerSpeed_ * (now < player.speedBoostUntil ? speedBoostMultiplier_ : 1.0);
      const double nextX = player.x + dx * speed * dt;
      const double nextY = player.y + dy * speed * dt;
      const bool phasing = now < player.shieldUntil;

      double candidateX = nextX;
      double candidateY = player.y;
      world_.clampToBounds(candidateX, candidateY);
      if (phasing || !world_.collides(candidateX, candidateY))
      {
        player.x = candidateX;
      }

      candidateX = player.x;
      candidateY = nextY;
      world_.clampToBounds(candidateX, candidateY);
      if (phasing || !world_.collides(candidateX, candidateY))
      {
        player.y = candidateY;
      }
    }

    if (intermission_)
    {
      return;
    }

    handleOrbPickupsLocked();
    handlePowerupPickupsLocked();
    handleControlZoneLocked(dt);
  }

  void GameServer::ensureOrbsLocked()
  {
    while (orbs_.size() < targetOrbCount_)
    {
      orbs_.push_back(spawnOrbLocked());
    }
  }

  void GameServer::ensurePowerupsLocked()
  {
    while (powerups_.size() < targetPowerupCount_)
    {
      powerups_.push_back(spawnPowerupLocked());
    }
  }

  void GameServer::updateRoundLocked()
  {
    const auto now = std::chrono::steady_clock::now();
    if (intermission_)
    {
      if (now >= intermissionUntil_)
      {
        startNextRoundLocked(now);
      }
      return;
    }

    if (now - roundStartedAt_ >= std::chrono::seconds(roundDurationSeconds_))
    {
      finishRoundLocked(now);
    }
  }

  void GameServer::finishRoundLocked(std::chrono::steady_clock::time_point now)
  {
    lastWinnerId_.clear();
    lastWinnerName_ = "No winner";
    lastWinnerScore_ = 0;

    for (const auto &[_, player] : players_)
    {
      if (lastWinnerId_.empty() || player.score > lastWinnerScore_)
      {
        lastWinnerId_ = player.id;
        lastWinnerName_ = player.name;
        lastWinnerScore_ = player.score;
      }
    }

    intermission_ = true;
    intermissionUntil_ = now + std::chrono::seconds(intermissionSeconds_);
    ++totalRoundsCompleted_;
    addEventLocked("round_end", lastWinnerName_ + " won round " + std::to_string(roundNumber_) + " with " + std::to_string(lastWinnerScore_) + " points");
  }

  void GameServer::startNextRoundLocked(std::chrono::steady_clock::time_point now)
  {
    intermission_ = false;
    roundStartedAt_ = now;
    ++roundNumber_;
    orbs_.clear();
    ensureOrbsLocked();
    powerups_.clear();
    ensurePowerupsLocked();

    for (auto &[_, player] : players_)
    {
      player.score = 0;
      player.orbQuestProgress = 0;
      player.controlCarry = 0.0;
      auto [x, y] = world_.randomSpawn(rng_);
      player.x = x;
      player.y = y;
      player.input = {};
      player.speedBoostUntil = now;
      player.dashReadyAt = now;
      player.shieldUntil = now;
      player.shieldReadyAt = now;
      player.magnetUntil = now;
      player.magnetReadyAt = now;
      player.facingX = 1.0;
      player.facingY = 0.0;
    }

    addEventLocked("round_start", "Round " + std::to_string(roundNumber_) + " started");
  }

  void GameServer::handleOrbPickupsLocked()
  {
    const double pickupRadius = World::playerRadius + orbRadius_;
    const double pickupRadiusSq = pickupRadius * pickupRadius;
    const double magnetRadiusSq = magnetRadius_ * magnetRadius_;
    const auto now = std::chrono::steady_clock::now();

    for (auto &[_, player] : players_)
    {
      for (auto &orb : orbs_)
      {
        const double dx = player.x - orb.x;
        const double dy = player.y - orb.y;
        const double distSq = dx * dx + dy * dy;
        if (now < player.magnetUntil && distSq < magnetRadiusSq && distSq > pickupRadiusSq)
        {
          const double dist = std::sqrt(distSq);
          const double pull = std::min(18.0, dist);
          orb.x += (dx / dist) * pull;
          orb.y += (dy / dist) * pull;
        }

        const double pickupDx = player.x - orb.x;
        const double pickupDy = player.y - orb.y;
        if (pickupDx * pickupDx + pickupDy * pickupDy <= pickupRadiusSq)
        {
          player.score += orb.value;
          player.orbQuestProgress += 1;
          ++totalOrbPickups_;
          addEventLocked("orb", player.name + " collected +" + std::to_string(orb.value));
          if (player.orbQuestProgress >= orbQuestGoal_)
          {
            player.orbQuestProgress = 0;
            player.score += orbQuestReward_;
            ++totalQuestsCompleted_;
            addEventLocked("quest", player.name + " completed Orb Run +" + std::to_string(orbQuestReward_));
          }
          orb = spawnOrbLocked();
        }
      }
    }
  }

  void GameServer::handlePowerupPickupsLocked()
  {
    const double pickupRadius = World::playerRadius + powerupRadius_;
    const double pickupRadiusSq = pickupRadius * pickupRadius;
    const auto now = std::chrono::steady_clock::now();

    for (auto &[_, player] : players_)
    {
      for (auto &powerup : powerups_)
      {
        const double dx = player.x - powerup.x;
        const double dy = player.y - powerup.y;
        if (dx * dx + dy * dy <= pickupRadiusSq)
        {
          player.speedBoostUntil = now + std::chrono::milliseconds(
                                             static_cast<int>(powerup.durationSeconds * 1000.0));
          ++totalPowerupsSinceStart_;
          addEventLocked("powerup", player.name + " grabbed speed boost");
          powerup = spawnPowerupLocked();
        }
      }
    }
  }

  void GameServer::handleControlZoneLocked(double dt)
  {
    const double zoneSq = controlZoneRadius_ * controlZoneRadius_;
    std::vector<Player *> occupants;

    for (auto &[_, player] : players_)
    {
      const double dx = player.x - controlZoneX_;
      const double dy = player.y - controlZoneY_;
      if (dx * dx + dy * dy > zoneSq)
      {
        player.controlCarry = 0.0;
        continue;
      }
      occupants.push_back(&player);
    }

    if (occupants.size() != 1)
    {
      for (Player *player : occupants)
      {
        player->controlCarry = 0.0;
      }
      return;
    }

    Player &holder = *occupants.front();
    holder.controlCarry += dt * controlPointsPerSecond_;
    const int wholePoints = static_cast<int>(holder.controlCarry);
    if (wholePoints > 0)
    {
      holder.score += wholePoints;
      totalControlZonePoints_ += static_cast<std::uint64_t>(wholePoints);
      holder.controlCarry -= wholePoints;
    }
  }

  void GameServer::applyDashLocked(Player &player, std::chrono::steady_clock::time_point now)
  {
    double dx = player.facingX;
    double dy = player.facingY;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0)
    {
      dx = 1.0;
      dy = 0.0;
    }
    else
    {
      dx /= len;
      dy /= len;
    }

    const bool phasing = now < player.shieldUntil;
    constexpr int steps = 8;
    const double step = dashDistance_ / static_cast<double>(steps);
    for (int i = 0; i < steps; ++i)
    {
      double candidateX = player.x + dx * step;
      double candidateY = player.y + dy * step;
      world_.clampToBounds(candidateX, candidateY);
      if (!phasing && world_.collides(candidateX, candidateY))
      {
        break;
      }
      player.x = candidateX;
      player.y = candidateY;
    }
  }

  void GameServer::addEventLocked(std::string type, std::string text)
  {
    eventHistory_.push_back({
        nextEventNumber_++,
        std::move(type),
        std::move(text),
        isoTimestampUtc(),
    });

    while (eventHistory_.size() > 24)
    {
      eventHistory_.pop_front();
    }
  }

  void GameServer::cleanupStaleLocked(std::vector<nlohmann::json> &leftEvents)
  {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = players_.begin(); it != players_.end();)
    {
      const bool expiredSession = it->second.session.expired();
      const bool stale = now - it->second.lastSeen > std::chrono::seconds(20);
      if (expiredSession || stale)
      {
        leftEvents.push_back({{"type", "player_left"}, {"id", it->second.id}});
        for (auto sit = sessionToPlayer_.begin(); sit != sessionToPlayer_.end();)
        {
          if (sit->second == it->second.id)
            sit = sessionToPlayer_.erase(sit);
          else
            ++sit;
        }
        it = players_.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  nlohmann::json GameServer::snapshotLocked() const
  {
    nlohmann::json players = nlohmann::json::array();
    for (const auto &[_, player] : players_)
    {
      players.push_back(playerToJson(player));
    }

    return {
        {"type", "snapshot"},
        {"players", players},
        {"orbs", orbsJsonLocked()},
        {"powerups", powerupsJsonLocked()},
        {"controlZone", controlZoneJson()},
        {"round", roundJsonLocked()},
        {"events", eventsJsonLocked()},
        {"serverTime", isoTimestampUtc()}};
  }

  nlohmann::json GameServer::orbsJsonLocked() const
  {
    nlohmann::json orbs = nlohmann::json::array();
    for (const auto &orb : orbs_)
    {
      orbs.push_back({
          {"id", orb.id},
          {"x", orb.x},
          {"y", orb.y},
          {"value", orb.value},
          {"color", orb.color}});
    }
    return orbs;
  }

  nlohmann::json GameServer::powerupsJsonLocked() const
  {
    nlohmann::json powerups = nlohmann::json::array();
    for (const auto &powerup : powerups_)
    {
      powerups.push_back({
          {"id", powerup.id},
          {"kind", powerup.kind},
          {"x", powerup.x},
          {"y", powerup.y},
          {"durationSeconds", powerup.durationSeconds},
          {"color", powerup.color}});
    }
    return powerups;
  }

  nlohmann::json GameServer::controlZoneJson() const
  {
    return {
        {"x", controlZoneX_},
        {"y", controlZoneY_},
        {"radius", controlZoneRadius_},
        {"pointsPerSecond", controlPointsPerSecond_}};
  }

  nlohmann::json GameServer::roundJsonLocked() const
  {
    const auto now = std::chrono::steady_clock::now();
    int secondsRemaining = 0;
    std::string phase = "active";

    if (intermission_)
    {
      phase = "intermission";
      secondsRemaining = static_cast<int>(
          std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(intermissionUntil_ - now).count()));
    }
    else
    {
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - roundStartedAt_).count();
      secondsRemaining = static_cast<int>(std::max<std::int64_t>(0, roundDurationSeconds_ - elapsed));
    }

    return {
        {"number", roundNumber_},
        {"phase", phase},
        {"secondsRemaining", secondsRemaining},
        {"durationSeconds", roundDurationSeconds_},
        {"intermissionSeconds", intermissionSeconds_},
        {"lastWinner", {
                           {"id", lastWinnerId_},
                           {"name", lastWinnerName_},
                           {"score", lastWinnerScore_},
                       }}};
  }

  nlohmann::json GameServer::eventsJsonLocked() const
  {
    nlohmann::json events = nlohmann::json::array();
    for (const auto &event : eventHistory_)
    {
      events.push_back({
          {"id", event.id},
          {"type", event.type},
          {"text", event.text},
          {"timestamp", event.timestamp}});
    }
    return events;
  }

  std::vector<GameServer::SessionPtr> GameServer::liveSessionsLocked() const
  {
    std::vector<SessionPtr> sessions;
    sessions.reserve(players_.size());
    for (const auto &[_, player] : players_)
    {
      if (auto s = player.session.lock())
      {
        sessions.push_back(std::move(s));
      }
    }
    return sessions;
  }

  void GameServer::send(ClientConnection *session, const nlohmann::json &message)
  {
    try
    {
      if (session && session->open.load() && session->send)
      {
        session->send(message.dump());
      }
    }
    catch (const std::exception &e)
    {
      std::cerr << "send failed: " << e.what() << '\n';
    }
  }

  void GameServer::broadcast(const nlohmann::json &message)
  {
    std::vector<SessionPtr> sessions;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sessions = liveSessionsLocked();
    }
    broadcastTo(sessions, message);
  }

  void GameServer::broadcastTo(const std::vector<SessionPtr> &sessions, const nlohmann::json &message)
  {
    const std::string payload = message.dump();
    for (const auto &session : sessions)
    {
      if (session && session->open.load() && session->send)
      {
        try
        {
          session->send(payload);
        }
        catch (const std::exception &e)
        {
          std::cerr << "broadcast failed: " << e.what() << '\n';
        }
      }
    }
  }

  std::string GameServer::randomColor()
  {
    static const std::vector<std::string> palette = {
        "#66ccff", "#ffcc66", "#7af59b", "#ff7aa8", "#c9a7ff",
        "#f5f06b", "#62e6d8", "#ff9d66", "#a6ff66", "#e6e6e6"};
    std::uniform_int_distribution<std::size_t> dist(0, palette.size() - 1);
    return palette[dist(rng_)];
  }

  GameServer::Orb GameServer::spawnOrbLocked()
  {
    static const std::vector<std::string> colors = {
        "#66ccff", "#ffcc66", "#7af59b", "#ff7aa8", "#f5f06b"};

    std::uniform_real_distribution<double> xdist(40.0, world_.width() - 40.0);
    std::uniform_real_distribution<double> ydist(40.0, world_.height() - 40.0);
    std::uniform_int_distribution<int> valueDist(0, 9);
    std::uniform_int_distribution<std::size_t> colorDist(0, colors.size() - 1);

    Orb orb;
    orb.id = "o-" + std::to_string(nextOrbNumber_++);
    orb.value = valueDist(rng_) == 0 ? 15 : 5;
    orb.color = orb.value > 5 ? "#f5f06b" : colors[colorDist(rng_)];

    for (int attempt = 0; attempt < 120; ++attempt)
    {
      const double x = xdist(rng_);
      const double y = ydist(rng_);
      if (!world_.collides(x, y, orbRadius_ + 8.0))
      {
        orb.x = x;
        orb.y = y;
        return orb;
      }
    }

    orb.x = world_.width() * 0.5;
    orb.y = world_.height() * 0.5 - 220.0;
    return orb;
  }

  GameServer::Powerup GameServer::spawnPowerupLocked()
  {
    std::uniform_real_distribution<double> xdist(60.0, world_.width() - 60.0);
    std::uniform_real_distribution<double> ydist(60.0, world_.height() - 60.0);

    Powerup powerup;
    powerup.id = "u-" + std::to_string(nextPowerupNumber_++);

    for (int attempt = 0; attempt < 120; ++attempt)
    {
      const double x = xdist(rng_);
      const double y = ydist(rng_);
      if (!world_.collides(x, y, powerupRadius_ + 8.0))
      {
        powerup.x = x;
        powerup.y = y;
        return powerup;
      }
    }

    powerup.x = world_.width() * 0.5 + 260.0;
    powerup.y = world_.height() * 0.5;
    return powerup;
  }

  nlohmann::json GameServer::healthJson() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        {"status", "ok"},
        {"service", "vix-arena"},
        {"players", players_.size()},
        {"uptimeSeconds", uptimeSeconds(startedAt_)}};
  }

  nlohmann::json GameServer::stateJson() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        {"service", "vix-arena"},
        {"players", players_.size()},
        {"world", worldSummary(world_)},
        {"orbs", orbsJsonLocked()},
        {"powerups", powerupsJsonLocked()},
        {"controlZone", controlZoneJson()},
        {"round", roundJsonLocked()},
        {"events", eventsJsonLocked()}};
  }

  nlohmann::json GameServer::statsJson() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        {"service", "vix-arena"},
        {"connectedPlayers", players_.size()},
        {"maxPlayers", maxPlayers_},
        {"uptimeSeconds", uptimeSeconds(startedAt_)},
        {"tickRateTarget", tickRateTarget_},
        {"totalConnectionsSinceStart", totalConnections_},
        {"totalChatMessagesSinceStart", totalChatMessages_},
        {"totalOrbPickupsSinceStart", totalOrbPickups_},
        {"totalControlZonePointsSinceStart", totalControlZonePoints_},
        {"totalPowerupsSinceStart", totalPowerupsSinceStart_},
        {"totalQuestsCompletedSinceStart", totalQuestsCompleted_},
        {"roundNumber", roundNumber_},
        {"totalRoundsCompletedSinceStart", totalRoundsCompleted_}};
  }
}
