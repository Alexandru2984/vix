#include "GameServer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <utility>

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

    double distanceSq(double ax, double ay, double bx, double by)
    {
      const double dx = ax - bx;
      const double dy = ay - by;
      return dx * dx + dy * dy;
    }

    std::uint64_t percentile(std::vector<std::uint64_t> values, double p)
    {
      if (values.empty())
      {
        return 0;
      }
      std::sort(values.begin(), values.end());
      const auto index = static_cast<std::size_t>(std::ceil((p / 100.0) * static_cast<double>(values.size())) - 1.0);
      return values[std::min(index, values.size() - 1)];
    }

    void appendMetric(std::ostringstream &out, const char *name, const char *help, const char *type, std::uint64_t value)
    {
      out << "# HELP " << name << ' ' << help << '\n';
      out << "# TYPE " << name << ' ' << type << '\n';
      out << name << ' ' << value << '\n';
    }

    bool jsonArrayContainsString(const nlohmann::json &array, std::string_view value)
    {
      if (!array.is_array())
      {
        return false;
      }
      return std::any_of(array.begin(), array.end(), [&](const nlohmann::json &item)
                         { return item.is_string() && item.get<std::string>() == value; });
    }

    std::uint64_t jsonUInt(const nlohmann::json &object, const char *key)
    {
      if (!object.contains(key))
      {
        return 0;
      }
      const auto &value = object.at(key);
      if (value.is_number_unsigned())
      {
        return value.get<std::uint64_t>();
      }
      if (value.is_number_integer())
      {
        const auto signedValue = value.get<std::int64_t>();
        return signedValue > 0 ? static_cast<std::uint64_t>(signedValue) : 0;
      }
      return 0;
    }

    int jsonInt(const nlohmann::json &object, const char *key)
    {
      if (!object.contains(key) || !object.at(key).is_number_integer())
      {
        return 0;
      }
      return object.at(key).get<int>();
    }

    nlohmann::json collectionDelta(const nlohmann::json &current, const nlohmann::json &previous)
    {
      std::unordered_map<std::string, nlohmann::json> previousById;
      if (previous.is_array())
      {
        for (const auto &item : previous)
        {
          if (item.contains("id") && item["id"].is_string())
          {
            previousById[item["id"].get<std::string>()] = item;
          }
        }
      }

      nlohmann::json upserts = nlohmann::json::array();
      std::unordered_map<std::string, bool> currentIds;
      if (current.is_array())
      {
        for (const auto &item : current)
        {
          if (!item.contains("id") || !item["id"].is_string())
          {
            continue;
          }
          const std::string id = item["id"].get<std::string>();
          currentIds[id] = true;
          const auto previousIt = previousById.find(id);
          if (previousIt == previousById.end() || previousIt->second != item)
          {
            upserts.push_back(item);
          }
        }
      }

      nlohmann::json removed = nlohmann::json::array();
      for (const auto &[id, _] : previousById)
      {
        if (!currentIds.contains(id))
        {
          removed.push_back(id);
        }
      }

      return {{"upserts", upserts}, {"removed", removed}};
    }

  }

  GameServer::GameServer(std::filesystem::path dataDir, std::string databaseUrl, std::filesystem::path migrationsDir)
      : rng_(std::random_device{}()),
        dataDir_(std::move(dataDir)),
        startedAt_(std::chrono::steady_clock::now())
  {
    if (!databaseUrl.empty())
    {
      persistence_ = std::make_unique<PersistenceStore>(std::move(databaseUrl), std::move(migrationsDir), matchHistoryLimit_);
    }
    if (!dataDir_.empty())
    {
      stateFile_ = dataDir_ / "vix-arena-state.json";
      loadPersistentStateLocked();
    }
    RoomState &publicRoom = roomStateLocked("public");
    ensureOrbsLocked(publicRoom);
    ensurePowerupsLocked(publicRoom);
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

  bool GameServer::onOpen(const std::shared_ptr<ClientConnection> &session)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string ip = session ? session->remoteAddress : "";
    if (!ip.empty() && connectionsByIp_[ip] >= maxConnectionsPerIp_)
    {
      ++totalRejectedConnections_;
      return false;
    }

    if (!ip.empty())
    {
      ++connectionsByIp_[ip];
    }
    SessionAbuseState abuse;
    abuse.messageTokens = wsMessageBurst_;
    abuse.lastRefill = std::chrono::steady_clock::now();
    sessionAbuse_[session.get()] = abuse;
    ++totalConnections_;
    return true;
  }

  void GameServer::onClose(ClientConnection *session)
  {
    nlohmann::json left;
    bool hasLeft = false;
    std::string roomCode = "public";

    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = sessionToPlayer_.find(session);
      auto abuseIt = sessionAbuse_.find(session);
      if (abuseIt != sessionAbuse_.end())
      {
        sessionAbuse_.erase(abuseIt);
      }
      if (session && !session->remoteAddress.empty())
      {
        auto ipIt = connectionsByIp_.find(session->remoteAddress);
        if (ipIt != connectionsByIp_.end())
        {
          if (ipIt->second > 1)
          {
            --ipIt->second;
          }
          else
          {
            connectionsByIp_.erase(ipIt);
          }
        }
      }
      if (it == sessionToPlayer_.end())
      {
        return;
      }
      else
      {
        const std::string id = it->second;
        sessionToPlayer_.erase(it);
        sessionProtocol_.erase(session);
        auto playerIt = players_.find(id);
        if (playerIt != players_.end())
        {
          roomCode = playerIt->second.roomCode;
          players_.erase(playerIt);
        }
        left = {{"type", "player_left"}, {"protocolVersion", protocolVersion}, {"serverTimeMs", unixTimeMs()}, {"id", id}, {"room", roomCode}};
        hasLeft = true;
        if (humanCountLocked(roomCode) == 0)
        {
          removeBotsLocked(roomCode);
        }
      }
    }

    if (hasLeft)
    {
      broadcastRoom(roomCode, left);
    }
  }

  void GameServer::onMessage(ClientConnection *session, const std::string &payload)
  {
    ++totalMessagesReceived_;
    totalMessageBytesReceived_ += payload.size();

    if (!consumeMessageToken(session))
    {
      send(session, errorMessage("message rate limit"));
      return;
    }

    if (payload.size() > maxWsPayloadBytes)
    {
      send(session, errorMessage("message too large"));
      if (recordInvalidMessage(session))
      {
        closeSession(session, "too many invalid messages");
      }
      return;
    }

    auto message = nlohmann::json::parse(payload, nullptr, false);
    if (message.is_discarded() || !message.is_object())
    {
      send(session, errorMessage("invalid JSON object"));
      if (recordInvalidMessage(session))
      {
        closeSession(session, "too many invalid messages");
      }
      return;
    }

    if (!message.contains("type") || !message["type"].is_string())
    {
      send(session, errorMessage("missing message type"));
      if (recordInvalidMessage(session))
      {
        closeSession(session, "too many invalid messages");
      }
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
      if (recordInvalidMessage(session))
      {
        closeSession(session, "too many invalid messages");
      }
    }
  }

  bool GameServer::consumeMessageToken(ClientConnection *session)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessionAbuse_.find(session);
    if (it == sessionAbuse_.end())
    {
      return true;
    }

    const auto now = std::chrono::steady_clock::now();
    SessionAbuseState &abuse = it->second;
    const std::chrono::duration<double> elapsed = now - abuse.lastRefill;
    abuse.lastRefill = now;
    abuse.messageTokens = std::min(wsMessageBurst_, abuse.messageTokens + elapsed.count() * wsMessageRefillPerSecond_);
    if (abuse.messageTokens < 1.0)
    {
      ++totalRateLimitRejects_;
      return false;
    }

    abuse.messageTokens -= 1.0;
    return true;
  }

  bool GameServer::recordInvalidMessage(ClientConnection *session)
  {
    ++totalProtocolViolations_;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessionAbuse_.find(session);
    if (it == sessionAbuse_.end())
    {
      return false;
    }
    ++it->second.invalidMessages;
    return it->second.invalidMessages >= maxInvalidMessagesPerConnection_;
  }

  void GameServer::closeSession(ClientConnection *session, const std::string &reason)
  {
    if (session && session->close)
    {
      session->close(reason);
    }
  }

  void GameServer::handleJoin(ClientConnection *session, const nlohmann::json &message)
  {
    std::string requested;
    if (message.contains("name") && message["name"].is_string())
    {
      requested = message["name"].get<std::string>();
    }
    std::string roomCode = "public";
    if (message.contains("room") && message["room"].is_string())
    {
      roomCode = sanitizeRoomCode(message["room"].get<std::string>());
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
              {"protocolVersion", protocolVersion},
              {"serverTimeMs", unixTimeMs()},
              {"id", pit->second.id},
              {"room", pit->second.roomCode},
              {"features", nlohmann::json::array({std::string(protocolFeatureSnapshotDelta)})},
              {"world", worldSummary(world_)}};
          snapshot = snapshotLocked(currentTick_, nextSnapshotId_++, pit->second.roomCode);
          auto state = sessionProtocol_.find(session);
          if (state != sessionProtocol_.end())
          {
            state->second.lastSnapshotId = snapshot.value("snapshotId", 0ULL);
            state->second.lastSnapshot = snapshot;
          }
          auto historyIt = chatHistoryByRoom_.find(pit->second.roomCode);
          if (historyIt != chatHistoryByRoom_.end())
            history.assign(historyIt->second.begin(), historyIt->second.end());
        }
      }
      else
      {
        ClientProtocolState protocolState;
        if (message.contains("protocolVersion") && message["protocolVersion"].is_number_integer())
        {
          protocolState.version = message["protocolVersion"].get<int>();
        }
        if (message.contains("supports"))
        {
          protocolState.supportsSnapshotDelta = protocolState.version >= protocolVersion &&
                                                jsonArrayContainsString(message["supports"], protocolFeatureSnapshotDelta);
        }

        if (humanCountLocked(roomCode) >= maxPlayers_)
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
          player.roomCode = roomCode;
          player.x = x;
          player.y = y;
          player.bot = false;
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
          sessionProtocol_[session] = protocolState;
          players_[id] = std::move(player);
          session->roomCode = roomCode;
          addEventLocked("join", playerName + " joined the arena", roomCode);
          ensureBotsLocked(std::chrono::steady_clock::now());

          welcome = {
              {"type", "welcome"},
              {"protocolVersion", protocolVersion},
              {"serverTimeMs", unixTimeMs()},
              {"id", id},
              {"room", roomCode},
              {"features", nlohmann::json::array({std::string(protocolFeatureSnapshotDelta)})},
              {"world", worldSummary(world_)}};
          joined = {{"type", "player_joined"}, {"protocolVersion", protocolVersion}, {"serverTimeMs", unixTimeMs()}, {"id", id}, {"name", playerName}, {"room", roomCode}};
          snapshot = snapshotLocked(currentTick_, nextSnapshotId_++, roomCode);
          if (auto state = sessionProtocol_.find(session); state != sessionProtocol_.end())
          {
            state->second.lastSnapshotId = snapshot.value("snapshotId", 0ULL);
            state->second.lastSnapshot = snapshot;
          }
          auto historyIt = chatHistoryByRoom_.find(roomCode);
          if (historyIt != chatHistoryByRoom_.end())
            history.assign(historyIt->second.begin(), historyIt->second.end());
          newPlayer = true;
        }
      }
    }

    if (!welcome.empty())
    {
      send(session, welcome);
      if (!history.empty())
      {
        send(session, {{"type", "chat_history"}, {"protocolVersion", protocolVersion}, {"serverTimeMs", unixTimeMs()}, {"messages", history}});
      }
      if (!snapshot.empty())
      {
        send(session, snapshot);
      }
    }

    if (newPlayer)
    {
      broadcastRoom(roomCode, joined);
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
      ++totalRateLimitRejects_;
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
        ++totalRateLimitRejects_;
        send(session, errorMessage("chat rate limit"));
        return;
      }

      pit->second.lastChat = now;
      pit->second.lastSeen = now;
      const std::string roomCode = pit->second.roomCode;
      outgoing = {
          {"type", "chat"},
          {"protocolVersion", protocolVersion},
          {"serverTimeMs", unixTimeMs()},
          {"room", roomCode},
          {"from", pit->second.name},
          {"message", text},
          {"timestamp", isoTimestampUtc()}};
      auto &history = chatHistoryByRoom_[roomCode];
      history.push_back(outgoing);
      while (history.size() > 50)
      {
        history.pop_front();
      }
      ++totalChatMessages_;
      accepted = true;
    }

    if (accepted)
    {
      broadcastRoom(outgoing.value("room", "public"), outgoing);
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

    nlohmann::json pong = {{"type", "pong"}, {"protocolVersion", protocolVersion}, {"serverTimeMs", unixTimeMs()}};
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
        ++player.roundAbilitiesUsed;
        addEventLocked("dash", player.name + " dashed", player.roomCode);
        outgoing = {{"type", "ability"}, {"protocolVersion", protocolVersion}, {"serverTimeMs", unixTimeMs()}, {"id", player.id}, {"room", player.roomCode}, {"ability", "dash"}, {"timestamp", isoTimestampUtc()}};
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
        ++player.roundAbilitiesUsed;
        addEventLocked("shield", player.name + " phased through obstacles", player.roomCode);
        outgoing = {{"type", "ability"}, {"protocolVersion", protocolVersion}, {"serverTimeMs", unixTimeMs()}, {"id", player.id}, {"room", player.roomCode}, {"ability", "shield"}, {"timestamp", isoTimestampUtc()}};
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
        ++player.roundAbilitiesUsed;
        addEventLocked("magnet", player.name + " activated magnet", player.roomCode);
        outgoing = {{"type", "ability"}, {"protocolVersion", protocolVersion}, {"serverTimeMs", unixTimeMs()}, {"id", player.id}, {"room", player.roomCode}, {"ability", "magnet"}, {"timestamp", isoTimestampUtc()}};
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
      broadcastRoom(outgoing.value("room", "public"), outgoing);
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
      const auto workStarted = now;
      const std::chrono::duration<double> elapsed = now - last;
      last = now;

      std::vector<nlohmann::json> leftEvents;
      std::vector<SessionPtr> sessions;
      std::vector<PreparedPayload> snapshotPayloads;

      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++currentTick_;
        step(std::min(elapsed.count(), 0.2));
        cleanupStaleLocked(leftEvents);
        sessions = liveSessionsLocked();
        std::unordered_map<std::string, std::vector<SessionPtr>> sessionsByRoom;
        for (const auto &session : sessions)
        {
          sessionsByRoom[session && !session->roomCode.empty() ? session->roomCode : "public"].push_back(session);
        }
        for (const auto &[roomCode, roomSessions] : sessionsByRoom)
        {
          const nlohmann::json snapshot = snapshotLocked(currentTick_, nextSnapshotId_++, roomCode);
          auto prepared = snapshotPayloadsLocked(roomSessions, snapshot);
          snapshotPayloads.insert(snapshotPayloads.end(), prepared.begin(), prepared.end());
        }
        recordTickDurationLocked(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - workStarted).count()));
      }

      sendPrepared(snapshotPayloads, true);
      for (const auto &event : leftEvents)
      {
        broadcastRoom(event.value("room", "public"), event);
      }

      std::this_thread::sleep_until(now + interval);
    }
  }

  void GameServer::recordTickDurationLocked(std::uint64_t durationUs)
  {
    ++totalTicks_;
    maxTickDurationUs_ = std::max(maxTickDurationUs_, durationUs);
    recentTickDurationsUs_.push_back(durationUs);
    while (recentTickDurationsUs_.size() > tickDurationSampleLimit_)
    {
      recentTickDurationsUs_.pop_front();
    }
  }

  GameServer::RoomState &GameServer::roomStateLocked(const std::string &roomCode)
  {
    RoomState &room = rooms_[roomCode.empty() ? "public" : roomCode];
    if (room.roundStartedAt == std::chrono::steady_clock::time_point{})
    {
      const auto now = std::chrono::steady_clock::now();
      room.roundStartedAt = now;
      room.intermissionUntil = now;
      ensureOrbsLocked(room);
      ensurePowerupsLocked(room);
    }
    return room;
  }

  const GameServer::RoomState *GameServer::roomStateLocked(const std::string &roomCode) const
  {
    auto it = rooms_.find(roomCode.empty() ? "public" : roomCode);
    return it == rooms_.end() ? nullptr : &it->second;
  }

  std::vector<std::string> GameServer::activeRoomCodesLocked() const
  {
    std::vector<std::string> rooms;
    std::unordered_map<std::string, bool> seen;
    for (const auto &[_, player] : players_)
    {
      if (!seen.contains(player.roomCode))
      {
        seen[player.roomCode] = true;
        rooms.push_back(player.roomCode);
      }
    }
    if (rooms.empty())
    {
      rooms.push_back("public");
    }
    return rooms;
  }

  void GameServer::step(double dt)
  {
    const auto now = std::chrono::steady_clock::now();
    for (const std::string &roomCode : activeRoomCodesLocked())
    {
      RoomState &room = roomStateLocked(roomCode);
      ensureOrbsLocked(room);
      ensurePowerupsLocked(room);
      updateRoundLocked(room, roomCode);
    }

    ensureBotsLocked(now);
    updateBotsLocked(now);

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

    for (const std::string &roomCode : activeRoomCodesLocked())
    {
      RoomState &room = roomStateLocked(roomCode);
      if (room.intermission)
      {
        continue;
      }
      handleOrbPickupsLocked(room, roomCode);
      handlePowerupPickupsLocked(room, roomCode);
      handleControlZoneLocked(room, roomCode, dt);
    }
  }

  void GameServer::ensureOrbsLocked(RoomState &room)
  {
    while (room.orbs.size() < targetOrbCount_)
    {
      room.orbs.push_back(spawnOrbLocked(room));
    }
  }

  void GameServer::ensurePowerupsLocked(RoomState &room)
  {
    while (room.powerups.size() < targetPowerupCount_)
    {
      room.powerups.push_back(spawnPowerupLocked(room));
    }
  }

  std::size_t GameServer::humanCountLocked() const
  {
    return static_cast<std::size_t>(std::count_if(players_.begin(), players_.end(), [](const auto &entry)
                                                 { return !entry.second.bot; }));
  }

  std::size_t GameServer::botCountLocked() const
  {
    return static_cast<std::size_t>(std::count_if(players_.begin(), players_.end(), [](const auto &entry)
                                                 { return entry.second.bot; }));
  }

  std::size_t GameServer::humanCountLocked(const std::string &roomCode) const
  {
    return static_cast<std::size_t>(std::count_if(players_.begin(), players_.end(), [&](const auto &entry)
                                                 { return !entry.second.bot && entry.second.roomCode == roomCode; }));
  }

  std::size_t GameServer::botCountLocked(const std::string &roomCode) const
  {
    return static_cast<std::size_t>(std::count_if(players_.begin(), players_.end(), [&](const auto &entry)
                                                 { return entry.second.bot && entry.second.roomCode == roomCode; }));
  }

  void GameServer::ensureBotsLocked(std::chrono::steady_clock::time_point now)
  {
    std::unordered_map<std::string, std::size_t> humanRooms;
    for (const auto &[_, player] : players_)
    {
      if (!player.bot)
      {
        ++humanRooms[player.roomCode];
      }
    }

    if (humanRooms.empty())
    {
      removeBotsLocked();
      return;
    }

    for (const auto &[roomCode, humans] : humanRooms)
    {
      const std::size_t desiredBots = std::min(maxBots_, targetPlayersWithBots_ > humans ? targetPlayersWithBots_ - humans : 0);
      while (botCountLocked(roomCode) < desiredBots && players_.size() < maxPlayers_)
      {
        Player bot = spawnBotLocked(now, roomCode);
        const std::string id = bot.id;
        const std::string name = bot.name;
        players_[id] = std::move(bot);
        addEventLocked("bot", name + " booted into the arena", roomCode);
      }

      while (botCountLocked(roomCode) > desiredBots)
      {
        auto it = std::find_if(players_.begin(), players_.end(), [&](const auto &entry)
                               { return entry.second.bot && entry.second.roomCode == roomCode; });
        if (it == players_.end())
          break;
        addEventLocked("bot", it->second.name + " left the arena", roomCode);
        players_.erase(it);
      }
    }

    for (auto it = players_.begin(); it != players_.end();)
    {
      if (it->second.bot && !humanRooms.contains(it->second.roomCode))
      {
        addEventLocked("bot", it->second.name + " left the arena", it->second.roomCode);
        it = players_.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  void GameServer::removeBotsLocked(const std::string &roomCode)
  {
    for (auto it = players_.begin(); it != players_.end();)
    {
      if (it->second.bot && (roomCode.empty() || it->second.roomCode == roomCode))
      {
        it = players_.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  Player GameServer::spawnBotLocked(std::chrono::steady_clock::time_point now, const std::string &roomCode)
  {
    auto [x, y] = world_.randomSpawn(rng_);
    const std::uint64_t n = nextBotNumber_++;

    Player bot;
    bot.id = "bot-" + std::to_string(n);
    bot.name = "Bot " + std::to_string(n);
    bot.color = randomColor();
    bot.roomCode = roomCode;
    bot.bot = true;
    bot.x = x;
    bot.y = y;
    bot.lastSeen = now;
    bot.lastInput = now;
    bot.lastChat = now;
    bot.speedBoostUntil = now;
    bot.dashReadyAt = now + std::chrono::milliseconds(700 + static_cast<int>((n % 4) * 220));
    bot.shieldUntil = now;
    bot.shieldReadyAt = now + std::chrono::milliseconds(1800 + static_cast<int>((n % 3) * 500));
    bot.magnetUntil = now;
    bot.magnetReadyAt = now + std::chrono::milliseconds(1200 + static_cast<int>((n % 5) * 350));
    bot.nextBotDecisionAt = now;
    bot.botTargetX = x;
    bot.botTargetY = y;
    chooseBotTargetLocked(bot, now);
    return bot;
  }

  void GameServer::chooseBotTargetLocked(Player &bot, std::chrono::steady_clock::time_point now)
  {
    RoomState &room = roomStateLocked(bot.roomCode);
    std::uniform_int_distribution<int> modeDist(0, 99);
    const int mode = modeDist(rng_);

    if (!room.orbs.empty() && mode < 68)
    {
      const auto best = std::min_element(room.orbs.begin(), room.orbs.end(), [&bot](const Orb &a, const Orb &b)
                                         { return distanceSq(bot.x, bot.y, a.x, a.y) < distanceSq(bot.x, bot.y, b.x, b.y); });
      bot.botTargetX = best->x;
      bot.botTargetY = best->y;
    }
    else if (!room.powerups.empty() && mode < 86)
    {
      std::uniform_int_distribution<std::size_t> dist(0, room.powerups.size() - 1);
      const auto &powerup = room.powerups[dist(rng_)];
      bot.botTargetX = powerup.x;
      bot.botTargetY = powerup.y;
    }
    else if (mode < 96)
    {
      bot.botTargetX = controlZoneX_;
      bot.botTargetY = controlZoneY_;
    }
    else
    {
      std::uniform_real_distribution<double> xdist(World::playerRadius + 20.0, world_.width() - World::playerRadius - 20.0);
      std::uniform_real_distribution<double> ydist(World::playerRadius + 20.0, world_.height() - World::playerRadius - 20.0);
      bot.botTargetX = xdist(rng_);
      bot.botTargetY = ydist(rng_);
    }

    std::uniform_int_distribution<int> delayDist(550, 1500);
    bot.nextBotDecisionAt = now + std::chrono::milliseconds(delayDist(rng_));
  }

  void GameServer::updateBotsLocked(std::chrono::steady_clock::time_point now)
  {
    for (auto &[_, bot] : players_)
    {
      if (!bot.bot)
        continue;

      bot.lastSeen = now;
      const double dx = bot.botTargetX - bot.x;
      const double dy = bot.botTargetY - bot.y;
      const double dist = std::sqrt(dx * dx + dy * dy);

      if (dist < 36.0 || now >= bot.nextBotDecisionAt)
      {
        chooseBotTargetLocked(bot, now);
      }

      const double tx = bot.botTargetX - bot.x;
      const double ty = bot.botTargetY - bot.y;
      bot.input.left = tx < -18.0;
      bot.input.right = tx > 18.0;
      bot.input.up = ty < -18.0;
      bot.input.down = ty > 18.0;

      if (dist > 300.0 && now >= bot.dashReadyAt)
      {
        bot.dashReadyAt = now + std::chrono::milliseconds(dashCooldownMs_ + 700);
        applyDashLocked(bot, now);
        ++bot.roundAbilitiesUsed;
        addEventLocked("dash", bot.name + " dashed", bot.roomCode);
      }
      RoomState &room = roomStateLocked(bot.roomCode);
      if (!room.orbs.empty() && now >= bot.magnetReadyAt)
      {
        const auto nearby = std::count_if(room.orbs.begin(), room.orbs.end(), [&bot](const Orb &orb)
                                          { return distanceSq(bot.x, bot.y, orb.x, orb.y) < magnetRadius_ * magnetRadius_; });
        if (nearby >= 2)
        {
          bot.magnetUntil = now + std::chrono::milliseconds(magnetDurationMs_);
          bot.magnetReadyAt = now + std::chrono::milliseconds(magnetCooldownMs_ + 900);
          ++bot.roundAbilitiesUsed;
          addEventLocked("magnet", bot.name + " activated magnet", bot.roomCode);
        }
      }
    }
  }

  void GameServer::updateRoundLocked(RoomState &room, const std::string &roomCode)
  {
    const auto now = std::chrono::steady_clock::now();
    if (room.intermission)
    {
      if (now >= room.intermissionUntil)
      {
        startNextRoundLocked(room, now, roomCode);
      }
      return;
    }

    if (now - room.roundStartedAt >= std::chrono::seconds(roundDurationSeconds_))
    {
      finishRoundLocked(room, now, roomCode);
    }
  }

  void GameServer::finishRoundLocked(RoomState &room, std::chrono::steady_clock::time_point now, const std::string &roomCode)
  {
    room.lastWinnerId.clear();
    room.lastWinnerName = "No winner";
    room.lastWinnerScore = 0;

    for (const auto &[_, player] : players_)
    {
      if (player.roomCode != roomCode)
      {
        continue;
      }
      if (room.lastWinnerId.empty() || player.score > room.lastWinnerScore)
      {
        room.lastWinnerId = player.id;
        room.lastWinnerName = player.name;
        room.lastWinnerScore = player.score;
      }
    }

    room.intermission = true;
    room.intermissionUntil = now + std::chrono::seconds(intermissionSeconds_);
    ++room.totalRoundsCompleted;
    ++totalRoundsCompleted_;
    recordRoundLocked(room, roomCode);
    savePersistentStateLocked();
    addEventLocked("round_end", room.lastWinnerName + " won round " + std::to_string(room.roundNumber) + " with " + std::to_string(room.lastWinnerScore) + " points", roomCode);
  }

  void GameServer::startNextRoundLocked(RoomState &room, std::chrono::steady_clock::time_point now, const std::string &roomCode)
  {
    room.intermission = false;
    room.roundStartedAt = now;
    ++room.roundNumber;
    room.orbs.clear();
    ensureOrbsLocked(room);
    room.powerups.clear();
    ensurePowerupsLocked(room);

    for (auto &[_, player] : players_)
    {
      if (player.roomCode != roomCode)
      {
        continue;
      }
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
      player.roundOrbPickups = 0;
      player.roundPowerups = 0;
      player.roundQuests = 0;
      player.roundControlZonePoints = 0;
      player.roundAbilitiesUsed = 0;
      player.facingX = 1.0;
      player.facingY = 0.0;
    }

    addEventLocked("round_start", "Round " + std::to_string(room.roundNumber) + " started", roomCode);
  }

  void GameServer::loadPersistentStateLocked()
  {
    if (stateFile_.empty() || !std::filesystem::exists(stateFile_))
    {
      return;
    }

    try
    {
      std::ifstream in(stateFile_);
      if (!in)
      {
        return;
      }

      const nlohmann::json state = nlohmann::json::parse(in, nullptr, false);
      if (state.is_discarded() || !state.is_object())
      {
        std::cerr << "ignoring invalid persistent state: " << stateFile_ << '\n';
        return;
      }

      leaderboard_.clear();
      if (state.contains("leaderboard") && state.at("leaderboard").is_array())
      {
        for (const auto &item : state.at("leaderboard"))
        {
          if (!item.is_object() || !item.contains("name") || !item.at("name").is_string())
          {
            continue;
          }

          LeaderboardEntry entry;
          entry.name = sanitizeDisplayName(item.at("name").get<std::string>());
          if (entry.name.empty())
          {
            continue;
          }
          entry.rounds = jsonUInt(item, "rounds");
          entry.wins = jsonUInt(item, "wins");
          entry.totalScore = jsonUInt(item, "totalScore");
          entry.bestScore = std::max(0, jsonInt(item, "bestScore"));
          if (item.contains("lastPlayedAt") && item.at("lastPlayedAt").is_string())
          {
            entry.lastPlayedAt = item.at("lastPlayedAt").get<std::string>();
          }
          leaderboard_[entry.name] = std::move(entry);
        }
      }

      matchHistory_.clear();
      if (state.contains("matches") && state.at("matches").is_array())
      {
        for (const auto &item : state.at("matches"))
        {
          if (!item.is_object())
          {
            continue;
          }
          matchHistory_.push_back(item);
          if (matchHistory_.size() >= matchHistoryLimit_)
          {
            break;
          }
        }
      }
    }
    catch (const std::exception &e)
    {
      std::cerr << "failed to load persistent state from " << stateFile_ << ": " << e.what() << '\n';
    }
  }

  void GameServer::savePersistentStateLocked() const
  {
    if (stateFile_.empty())
    {
      return;
    }

    try
    {
      std::filesystem::create_directories(dataDir_);
      const std::filesystem::path tmp = stateFile_.string() + ".tmp";
      nlohmann::json state = {
          {"schemaVersion", 1},
          {"service", "vix-arena"},
          {"updatedAt", isoTimestampUtc()},
          {"leaderboard", leaderboardJsonLocked(leaderboard_.size()).at("entries")},
          {"matches", matchesJsonLocked(matchHistoryLimit_).at("matches")}};

      {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
        {
          std::cerr << "failed to open persistent state temp file: " << tmp << '\n';
          return;
        }
        out << state.dump(2) << '\n';
      }

      std::error_code ec;
      std::filesystem::rename(tmp, stateFile_, ec);
      if (ec)
      {
        std::filesystem::remove(stateFile_, ec);
        std::filesystem::rename(tmp, stateFile_, ec);
      }
      if (ec)
      {
        std::cerr << "failed to replace persistent state file " << stateFile_ << ": " << ec.message() << '\n';
      }
    }
    catch (const std::exception &e)
    {
      std::cerr << "failed to save persistent state to " << stateFile_ << ": " << e.what() << '\n';
    }
  }

  void GameServer::recordRoundLocked(RoomState &room, const std::string &roomCode)
  {
    if (humanCountLocked(roomCode) == 0 && botCountLocked(roomCode) == 0)
    {
      return;
    }

    const std::string endedAt = isoTimestampUtc();
    std::vector<nlohmann::json> participants;
    participants.reserve(players_.size());
    const MatchRecord dbRecord = matchRecordLocked(room, endedAt, roomCode);

    for (const auto &[_, player] : players_)
    {
      if (player.roomCode != roomCode)
      {
        continue;
      }
      participants.push_back({
          {"id", player.id},
          {"name", player.name},
          {"score", player.score},
          {"bot", player.bot},
          {"winner", player.id == room.lastWinnerId},
          {"orbPickups", player.roundOrbPickups},
          {"powerups", player.roundPowerups},
          {"quests", player.roundQuests},
          {"controlZonePoints", player.roundControlZonePoints},
          {"abilitiesUsed", player.roundAbilitiesUsed},
      });

      if (!player.bot)
      {
        const std::string name = sanitizeDisplayName(player.name);
        if (name.empty())
        {
          continue;
        }
        LeaderboardEntry &entry = leaderboard_[name];
        entry.name = name;
        ++entry.rounds;
        if (player.id == room.lastWinnerId)
        {
          ++entry.wins;
        }
        entry.totalScore += static_cast<std::uint64_t>(std::max(0, player.score));
        entry.bestScore = std::max(entry.bestScore, player.score);
        entry.lastPlayedAt = endedAt;
      }
    }

    std::sort(participants.begin(), participants.end(), [](const nlohmann::json &left, const nlohmann::json &right)
              {
                const int leftScore = left.value("score", 0);
                const int rightScore = right.value("score", 0);
                if (leftScore != rightScore)
                  return leftScore > rightScore;
                return left.value("name", "") < right.value("name", "");
              });

    matchHistory_.push_front({
        {"round", room.roundNumber},
        {"room", roomCode},
        {"endedAt", endedAt},
        {"winner", {
                       {"id", room.lastWinnerId},
                       {"name", room.lastWinnerName},
                       {"score", room.lastWinnerScore},
                   }},
        {"participants", participants},
    });
    while (matchHistory_.size() > matchHistoryLimit_)
    {
      matchHistory_.pop_back();
    }

    if (persistence_ && persistence_->enabled())
    {
      persistence_->enqueueMatch(dbRecord);
    }
  }

  MatchRecord GameServer::matchRecordLocked(const RoomState &room, const std::string &endedAt, const std::string &roomCode) const
  {
    MatchRecord record;
    record.roomCode = roomCode.empty() ? "public" : roomCode;
    record.round = room.roundNumber;
    record.endedAt = endedAt;
    record.winnerId = room.lastWinnerId;
    record.winnerName = room.lastWinnerName;
    record.winnerScore = room.lastWinnerScore;
    record.durationSeconds = roundDurationSeconds_;
    record.participants.reserve(players_.size());

    for (const auto &[_, player] : players_)
    {
      if (player.roomCode != roomCode)
      {
        continue;
      }
      record.participants.push_back({
          player.id,
          player.name,
          player.bot,
          player.score,
          player.roundOrbPickups,
          player.roundPowerups,
          player.roundQuests,
          player.roundControlZonePoints,
          player.roundAbilitiesUsed,
          player.id == room.lastWinnerId});
    }

    std::sort(record.participants.begin(), record.participants.end(), [](const MatchParticipantRecord &left, const MatchParticipantRecord &right)
              {
                if (left.score != right.score)
                  return left.score > right.score;
                return left.name < right.name;
              });
    return record;
  }

  nlohmann::json GameServer::leaderboardJsonLocked(std::size_t limit, const std::string &roomCode) const
  {
    std::unordered_map<std::string, LeaderboardEntry> roomEntries;
    const std::unordered_map<std::string, LeaderboardEntry> *source = &leaderboard_;
    if (!roomCode.empty())
    {
      for (const auto &match : matchHistory_)
      {
        const std::string matchRoom = match.value("room", "public");
        if (matchRoom != roomCode || !match.contains("participants") || !match.at("participants").is_array())
        {
          continue;
        }
        const std::string endedAt = match.value("endedAt", "");
        for (const auto &participant : match.at("participants"))
        {
          if (participant.value("bot", false))
          {
            continue;
          }
          const std::string name = sanitizeDisplayName(participant.value("name", ""));
          if (name.empty())
          {
            continue;
          }
          LeaderboardEntry &entry = roomEntries[name];
          entry.name = name;
          ++entry.rounds;
          if (participant.value("winner", false))
          {
            ++entry.wins;
          }
          const int score = participant.value("score", 0);
          entry.totalScore += static_cast<std::uint64_t>(std::max(0, score));
          entry.bestScore = std::max(entry.bestScore, score);
          entry.lastPlayedAt = std::max(entry.lastPlayedAt, endedAt);
        }
      }
      source = &roomEntries;
    }

    std::vector<LeaderboardEntry> entries;
    entries.reserve(source->size());
    for (const auto &[_, entry] : *source)
    {
      entries.push_back(entry);
    }

    std::sort(entries.begin(), entries.end(), [](const LeaderboardEntry &left, const LeaderboardEntry &right)
              {
                if (left.wins != right.wins)
                  return left.wins > right.wins;
                if (left.bestScore != right.bestScore)
                  return left.bestScore > right.bestScore;
                if (left.totalScore != right.totalScore)
                  return left.totalScore > right.totalScore;
                return left.name < right.name;
              });

    nlohmann::json result = {
        {"service", "vix-arena"},
        {"source", "json"},
        {"room", roomCode.empty() ? nlohmann::json(nullptr) : nlohmann::json(roomCode)},
        {"updatedAt", isoTimestampUtc()},
        {"entries", nlohmann::json::array()},
    };
    const std::size_t count = std::min(limit, entries.size());
    for (std::size_t i = 0; i < count; ++i)
    {
      const auto &entry = entries[i];
      result["entries"].push_back({
          {"rank", i + 1},
          {"name", entry.name},
          {"rounds", entry.rounds},
          {"wins", entry.wins},
          {"totalScore", entry.totalScore},
          {"averageScore", entry.rounds == 0 ? 0.0 : static_cast<double>(entry.totalScore) / static_cast<double>(entry.rounds)},
          {"bestScore", entry.bestScore},
          {"lastPlayedAt", entry.lastPlayedAt},
      });
    }
    return result;
  }

  nlohmann::json GameServer::matchesJsonLocked(std::size_t limit, const std::string &roomCode) const
  {
    nlohmann::json result = {
        {"service", "vix-arena"},
        {"source", "json"},
        {"room", roomCode.empty() ? nlohmann::json(nullptr) : nlohmann::json(roomCode)},
        {"updatedAt", isoTimestampUtc()},
        {"matches", nlohmann::json::array()},
    };
    for (const auto &match : matchHistory_)
    {
      if (!roomCode.empty() && match.value("room", "public") != roomCode)
      {
        continue;
      }
      result["matches"].push_back(match);
      if (result["matches"].size() >= limit)
      {
        break;
      }
    }
    return result;
  }

  void GameServer::handleOrbPickupsLocked(RoomState &room, const std::string &roomCode)
  {
    const double pickupRadius = World::playerRadius + orbRadius_;
    const double pickupRadiusSq = pickupRadius * pickupRadius;
    const double magnetRadiusSq = magnetRadius_ * magnetRadius_;
    const auto now = std::chrono::steady_clock::now();

    for (auto &[_, player] : players_)
    {
      if (player.roomCode != roomCode)
      {
        continue;
      }
      for (auto &orb : room.orbs)
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
          ++player.roundOrbPickups;
          ++totalOrbPickups_;
          addEventLocked("orb", player.name + " collected +" + std::to_string(orb.value), player.roomCode);
          if (player.orbQuestProgress >= orbQuestGoal_)
          {
            player.orbQuestProgress = 0;
            player.score += orbQuestReward_;
            ++player.roundQuests;
            ++totalQuestsCompleted_;
            addEventLocked("quest", player.name + " completed Orb Run +" + std::to_string(orbQuestReward_), player.roomCode);
          }
          orb = spawnOrbLocked(room);
        }
      }
    }
  }

  void GameServer::handlePowerupPickupsLocked(RoomState &room, const std::string &roomCode)
  {
    const double pickupRadius = World::playerRadius + powerupRadius_;
    const double pickupRadiusSq = pickupRadius * pickupRadius;
    const auto now = std::chrono::steady_clock::now();

    for (auto &[_, player] : players_)
    {
      if (player.roomCode != roomCode)
      {
        continue;
      }
      for (auto &powerup : room.powerups)
      {
        const double dx = player.x - powerup.x;
        const double dy = player.y - powerup.y;
        if (dx * dx + dy * dy <= pickupRadiusSq)
        {
          player.speedBoostUntil = now + std::chrono::milliseconds(
                                             static_cast<int>(powerup.durationSeconds * 1000.0));
          ++player.roundPowerups;
          ++totalPowerupsSinceStart_;
          addEventLocked("powerup", player.name + " grabbed speed boost", player.roomCode);
          powerup = spawnPowerupLocked(room);
        }
      }
    }
  }

  void GameServer::handleControlZoneLocked(RoomState &, const std::string &roomCode, double dt)
  {
    const double zoneSq = controlZoneRadius_ * controlZoneRadius_;
    std::unordered_map<std::string, std::vector<Player *>> occupantsByRoom;

    for (auto &[_, player] : players_)
    {
      if (player.roomCode != roomCode)
      {
        continue;
      }
      const double dx = player.x - controlZoneX_;
      const double dy = player.y - controlZoneY_;
      if (dx * dx + dy * dy > zoneSq)
      {
        player.controlCarry = 0.0;
        continue;
      }
      occupantsByRoom[player.roomCode].push_back(&player);
    }

    for (auto &[_, occupants] : occupantsByRoom)
    {
      if (occupants.size() != 1)
      {
        for (Player *player : occupants)
        {
          player->controlCarry = 0.0;
        }
        continue;
      }

      Player &holder = *occupants.front();
      holder.controlCarry += dt * controlPointsPerSecond_;
      const int wholePoints = static_cast<int>(holder.controlCarry);
      if (wholePoints > 0)
      {
        holder.score += wholePoints;
        totalControlZonePoints_ += static_cast<std::uint64_t>(wholePoints);
        holder.roundControlZonePoints += wholePoints;
        holder.controlCarry -= wholePoints;
      }
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

  void GameServer::addEventLocked(std::string type, std::string text, std::string roomCode)
  {
    RoomState &room = roomStateLocked(roomCode);
    const std::string code = roomCode.empty() ? "public" : roomCode;
    room.eventHistory.push_back({
        room.nextEventNumber++,
        code,
        std::move(type),
        std::move(text),
        isoTimestampUtc(),
    });

    while (room.eventHistory.size() > 24)
    {
      room.eventHistory.pop_front();
    }
  }

  void GameServer::cleanupStaleLocked(std::vector<nlohmann::json> &leftEvents)
  {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = players_.begin(); it != players_.end();)
    {
      if (it->second.bot)
      {
        ++it;
        continue;
      }
      const bool expiredSession = it->second.session.expired();
      const bool stale = now - it->second.lastSeen > std::chrono::seconds(20);
      if (expiredSession || stale)
      {
        const std::string roomCode = it->second.roomCode;
        leftEvents.push_back({{"type", "player_left"}, {"protocolVersion", protocolVersion}, {"serverTimeMs", unixTimeMs()}, {"id", it->second.id}, {"room", roomCode}});
        for (auto sit = sessionToPlayer_.begin(); sit != sessionToPlayer_.end();)
        {
          if (sit->second == it->second.id)
          {
            sessionProtocol_.erase(sit->first);
            sit = sessionToPlayer_.erase(sit);
          }
          else
          {
            ++sit;
          }
        }
        it = players_.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  nlohmann::json GameServer::snapshotLocked(std::uint64_t tick, std::uint64_t snapshotId, const std::string &roomCode) const
  {
    nlohmann::json players = nlohmann::json::array();
    for (const auto &[_, player] : players_)
    {
      if (player.roomCode == roomCode)
      {
        players.push_back(playerToJson(player));
      }
    }
    const RoomState *room = roomStateLocked(roomCode);
    const RoomState emptyRoom;
    const RoomState &roomRef = room ? *room : emptyRoom;

    return {
        {"type", "snapshot"},
        {"protocolVersion", protocolVersion},
        {"snapshotId", snapshotId},
        {"baseSnapshotId", nullptr},
        {"tick", tick},
        {"full", true},
        {"room", roomCode},
        {"players", players},
        {"orbs", orbsJsonLocked(roomRef)},
        {"powerups", powerupsJsonLocked(roomRef)},
        {"controlZone", controlZoneJson()},
        {"round", roundJsonLocked(roomRef)},
        {"events", eventsJsonLocked(roomRef)},
        {"serverTimeMs", unixTimeMs()},
        {"serverTime", isoTimestampUtc()}};
  }

  nlohmann::json GameServer::snapshotDeltaLocked(const nlohmann::json &current, const nlohmann::json &previous, std::uint64_t baseSnapshotId) const
  {
    const auto players = collectionDelta(current.value("players", nlohmann::json::array()), previous.value("players", nlohmann::json::array()));
    const auto orbs = collectionDelta(current.value("orbs", nlohmann::json::array()), previous.value("orbs", nlohmann::json::array()));
    const auto powerups = collectionDelta(current.value("powerups", nlohmann::json::array()), previous.value("powerups", nlohmann::json::array()));
    const auto events = collectionDelta(current.value("events", nlohmann::json::array()), previous.value("events", nlohmann::json::array()));

    nlohmann::json delta = {
        {"type", "snapshot_delta"},
        {"protocolVersion", protocolVersion},
        {"snapshotId", current.value("snapshotId", 0ULL)},
        {"baseSnapshotId", baseSnapshotId},
        {"tick", current.value("tick", 0ULL)},
        {"full", false},
        {"room", current.value("room", "public")},
        {"serverTimeMs", current.value("serverTimeMs", unixTimeMs())},
        {"players", players.at("upserts")},
        {"removedPlayers", players.at("removed")},
        {"orbs", orbs.at("upserts")},
        {"removedOrbs", orbs.at("removed")},
        {"powerups", powerups.at("upserts")},
        {"removedPowerups", powerups.at("removed")},
        {"events", events.at("upserts")},
        {"removedEvents", events.at("removed")}};

    if (current.value("controlZone", nlohmann::json::object()) != previous.value("controlZone", nlohmann::json::object()))
    {
      delta["controlZone"] = current.at("controlZone");
    }
    if (current.value("round", nlohmann::json::object()) != previous.value("round", nlohmann::json::object()))
    {
      delta["round"] = current.at("round");
    }

    return delta;
  }

  nlohmann::json GameServer::orbsJsonLocked(const RoomState &room) const
  {
    nlohmann::json orbs = nlohmann::json::array();
    for (const auto &orb : room.orbs)
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

  nlohmann::json GameServer::powerupsJsonLocked(const RoomState &room) const
  {
    nlohmann::json powerups = nlohmann::json::array();
    for (const auto &powerup : room.powerups)
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

  nlohmann::json GameServer::roundJsonLocked(const RoomState &room) const
  {
    const auto now = std::chrono::steady_clock::now();
    int secondsRemaining = 0;
    std::string phase = "active";

    if (room.intermission)
    {
      phase = "intermission";
      secondsRemaining = static_cast<int>(
          std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(room.intermissionUntil - now).count()));
    }
    else
    {
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - room.roundStartedAt).count();
      secondsRemaining = static_cast<int>(std::max<std::int64_t>(0, roundDurationSeconds_ - elapsed));
    }

    return {
        {"number", room.roundNumber},
        {"phase", phase},
        {"secondsRemaining", secondsRemaining},
        {"durationSeconds", roundDurationSeconds_},
        {"intermissionSeconds", intermissionSeconds_},
        {"lastWinner", {
                           {"id", room.lastWinnerId},
                           {"name", room.lastWinnerName},
                           {"score", room.lastWinnerScore},
                       }}};
  }

  nlohmann::json GameServer::eventsJsonLocked(const RoomState &room) const
  {
    nlohmann::json events = nlohmann::json::array();
    for (const auto &event : room.eventHistory)
    {
      events.push_back({
          {"id", event.id},
          {"room", event.roomCode},
          {"type", event.type},
          {"text", event.text},
          {"timestamp", event.timestamp}});
    }
    return events;
  }

  std::vector<GameServer::SessionPtr> GameServer::liveSessionsLocked(const std::string &roomCode) const
  {
    std::vector<SessionPtr> sessions;
    sessions.reserve(players_.size());
    for (const auto &[_, player] : players_)
    {
      if (!roomCode.empty() && player.roomCode != roomCode)
      {
        continue;
      }
      if (auto s = player.session.lock())
      {
        sessions.push_back(std::move(s));
      }
    }
    return sessions;
  }

  std::vector<GameServer::PreparedPayload> GameServer::snapshotPayloadsLocked(const std::vector<SessionPtr> &sessions, const nlohmann::json &snapshot)
  {
    std::vector<PreparedPayload> payloads;
    payloads.reserve(sessions.size());
    const std::string fullPayload = snapshot.dump();

    for (const auto &session : sessions)
    {
      if (!session)
      {
        continue;
      }

      std::string payload = fullPayload;
      auto stateIt = sessionProtocol_.find(session.get());
      if (stateIt != sessionProtocol_.end())
      {
        ClientProtocolState &state = stateIt->second;
        if (state.supportsSnapshotDelta && state.lastSnapshotId > 0 && state.lastSnapshot.is_object())
        {
          const nlohmann::json delta = snapshotDeltaLocked(snapshot, state.lastSnapshot, state.lastSnapshotId);
          const std::string deltaPayload = delta.dump();
          if (deltaPayload.size() < fullPayload.size())
          {
            payload = deltaPayload;
          }
        }

        state.lastSnapshotId = snapshot.value("snapshotId", 0ULL);
        state.lastSnapshot = snapshot;
      }

      payloads.emplace_back(session, std::move(payload));
    }

    return payloads;
  }

  void GameServer::send(ClientConnection *session, const nlohmann::json &message)
  {
    try
    {
      if (session && session->open.load() && session->send)
      {
        const std::string payload = message.dump();
        ++totalMessagesSent_;
        totalMessageBytesSent_ += payload.size();
        if (message.value("type", "") == "error")
        {
          ++totalRejectedMessages_;
        }
        if (message.value("type", "") == "snapshot")
        {
          ++totalSnapshotsSent_;
          totalSnapshotBytesSent_ += payload.size();
        }
        else if (message.value("type", "") == "snapshot_delta")
        {
          ++totalSnapshotDeltasSent_;
          totalSnapshotDeltaBytesSent_ += payload.size();
        }
        session->send(payload);
      }
    }
    catch (const std::exception &e)
    {
      ++totalSendFailures_;
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

  void GameServer::broadcastRoom(const std::string &roomCode, const nlohmann::json &message)
  {
    std::vector<SessionPtr> sessions;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sessions = liveSessionsLocked(roomCode.empty() ? "public" : roomCode);
    }
    broadcastTo(sessions, message);
  }

  void GameServer::broadcastTo(const std::vector<SessionPtr> &sessions, const nlohmann::json &message)
  {
    const std::string payload = message.dump();
    const std::string type = message.value("type", "");
    const bool snapshot = type == "snapshot";
    const bool snapshotDelta = type == "snapshot_delta";
    for (const auto &session : sessions)
    {
      if (session && session->open.load() && session->send)
      {
        try
        {
          ++totalMessagesSent_;
          totalMessageBytesSent_ += payload.size();
          if (snapshot)
          {
            ++totalSnapshotsSent_;
            totalSnapshotBytesSent_ += payload.size();
          }
          else if (snapshotDelta)
          {
            ++totalSnapshotDeltasSent_;
            totalSnapshotDeltaBytesSent_ += payload.size();
          }
          session->send(payload);
        }
        catch (const std::exception &e)
        {
          ++totalSendFailures_;
          std::cerr << "broadcast failed: " << e.what() << '\n';
        }
      }
    }
  }

  void GameServer::sendPrepared(const std::vector<PreparedPayload> &payloads, bool snapshotLike)
  {
    for (const auto &[session, payload] : payloads)
    {
      if (!session || !session->open.load() || !session->send)
      {
        continue;
      }

      try
      {
        ++totalMessagesSent_;
        totalMessageBytesSent_ += payload.size();
        if (snapshotLike)
        {
          if (payload.find("\"type\":\"snapshot_delta\"") != std::string::npos)
          {
            ++totalSnapshotDeltasSent_;
            totalSnapshotDeltaBytesSent_ += payload.size();
          }
          else
          {
            ++totalSnapshotsSent_;
            totalSnapshotBytesSent_ += payload.size();
          }
        }
        session->send(payload);
      }
      catch (const std::exception &e)
      {
        ++totalSendFailures_;
        std::cerr << "send failed: " << e.what() << '\n';
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

  GameServer::Orb GameServer::spawnOrbLocked(RoomState &room)
  {
    static const std::vector<std::string> colors = {
        "#66ccff", "#ffcc66", "#7af59b", "#ff7aa8", "#f5f06b"};

    std::uniform_real_distribution<double> xdist(40.0, world_.width() - 40.0);
    std::uniform_real_distribution<double> ydist(40.0, world_.height() - 40.0);
    std::uniform_int_distribution<int> valueDist(0, 9);
    std::uniform_int_distribution<std::size_t> colorDist(0, colors.size() - 1);

    Orb orb;
    orb.id = "o-" + std::to_string(room.nextOrbNumber++);
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

  GameServer::Powerup GameServer::spawnPowerupLocked(RoomState &room)
  {
    std::uniform_real_distribution<double> xdist(60.0, world_.width() - 60.0);
    std::uniform_real_distribution<double> ydist(60.0, world_.height() - 60.0);

    Powerup powerup;
    powerup.id = "u-" + std::to_string(room.nextPowerupNumber++);

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
        {"humans", humanCountLocked()},
        {"bots", botCountLocked()},
        {"uptimeSeconds", uptimeSeconds(startedAt_)}};
  }

  nlohmann::json GameServer::readyJson() const
  {
    const PersistenceStatus persistenceStatus = persistence_ ? persistence_->status() : PersistenceStatus{};
    const bool persistenceReady = !persistenceStatus.configured || persistenceStatus.enabled;
    return {
        {"status", persistenceReady ? "ok" : "degraded"},
        {"service", "vix-arena"},
        {"ready", persistenceReady},
        {"persistence", {
                            {"postgresConfigured", persistenceStatus.configured},
                            {"postgresEnabled", persistenceStatus.enabled},
                            {"postgresSchemaVersion", persistenceStatus.schemaVersion},
                            {"postgresQueuedWrites", persistenceStatus.queuedWrites},
                            {"postgresFailedWrites", persistenceStatus.failedWrites},
                            {"postgresLastError", persistenceStatus.lastError.empty() ? "" : "see service logs"},
                        }}};
  }

  nlohmann::json GameServer::stateJson() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const RoomState *publicRoom = roomStateLocked("public");
    const RoomState emptyRoom;
    const RoomState &roomRef = publicRoom ? *publicRoom : emptyRoom;
    return {
        {"service", "vix-arena"},
        {"players", players_.size()},
        {"humans", humanCountLocked()},
        {"bots", botCountLocked()},
        {"world", worldSummary(world_)},
        {"orbs", orbsJsonLocked(roomRef)},
        {"powerups", powerupsJsonLocked(roomRef)},
        {"controlZone", controlZoneJson()},
        {"round", roundJsonLocked(roomRef)},
        {"events", eventsJsonLocked(roomRef)}};
  }

  nlohmann::json GameServer::statsJson() const
  {
    std::vector<std::uint64_t> tickSamples;
    tickSamples.reserve(tickDurationSampleLimit_);
    const PersistenceStatus persistenceStatus = persistence_ ? persistence_->status() : PersistenceStatus{};
    std::lock_guard<std::mutex> lock(mutex_);
    tickSamples.assign(recentTickDurationsUs_.begin(), recentTickDurationsUs_.end());
    const RoomState *publicRoom = roomStateLocked("public");
    const std::uint64_t publicRoundNumber = publicRoom ? publicRoom->roundNumber : 1;
    return {
        {"service", "vix-arena"},
        {"connectedPlayers", players_.size()},
        {"humanPlayers", humanCountLocked()},
        {"botPlayers", botCountLocked()},
        {"maxPlayers", maxPlayers_},
        {"uptimeSeconds", uptimeSeconds(startedAt_)},
        {"tickRateTarget", tickRateTarget_},
        {"protocolVersion", protocolVersion},
        {"totalConnectionsSinceStart", totalConnections_},
        {"totalChatMessagesSinceStart", totalChatMessages_},
        {"totalOrbPickupsSinceStart", totalOrbPickups_},
        {"totalControlZonePointsSinceStart", totalControlZonePoints_},
        {"totalPowerupsSinceStart", totalPowerupsSinceStart_},
        {"totalQuestsCompletedSinceStart", totalQuestsCompleted_},
        {"roundNumber", publicRoundNumber},
        {"totalRoundsCompletedSinceStart", totalRoundsCompleted_},
        {"persistence", {
                            {"enabled", !stateFile_.empty() || persistenceStatus.enabled},
                            {"jsonFileEnabled", !stateFile_.empty()},
                            {"postgresConfigured", persistenceStatus.configured},
                            {"postgresEnabled", persistenceStatus.enabled},
                            {"postgresQueuedWrites", persistenceStatus.queuedWrites},
                            {"postgresSavedMatches", persistenceStatus.savedMatches},
                            {"postgresFailedWrites", persistenceStatus.failedWrites},
                            {"postgresSchemaVersion", persistenceStatus.schemaVersion},
                            {"postgresLastError", persistenceStatus.lastError.empty() ? "" : "see service logs"},
                            {"leaderboardEntries", leaderboard_.size()},
                            {"matchHistorySize", matchHistory_.size()},
                        }},
        {"totalTicksSinceStart", totalTicks_},
        {"tickDurationUs", {
                               {"p50", percentile(tickSamples, 50.0)},
                               {"p95", percentile(tickSamples, 95.0)},
                               {"p99", percentile(tickSamples, 99.0)},
                               {"max", maxTickDurationUs_},
                           }},
        {"websocket", {
                          {"activeConnections", sessionAbuse_.size()},
                          {"activeRemoteAddresses", connectionsByIp_.size()},
                          {"maxConnectionsPerIp", maxConnectionsPerIp_},
                          {"messageBurst", wsMessageBurst_},
                          {"messageRefillPerSecond", wsMessageRefillPerSecond_},
                          {"messagesReceived", totalMessagesReceived_.load()},
                          {"messageBytesReceived", totalMessageBytesReceived_.load()},
                          {"messagesSent", totalMessagesSent_.load()},
                          {"messageBytesSent", totalMessageBytesSent_.load()},
                          {"snapshotsSent", totalSnapshotsSent_.load()},
                          {"snapshotBytesSent", totalSnapshotBytesSent_.load()},
                          {"snapshotDeltasSent", totalSnapshotDeltasSent_.load()},
                          {"snapshotDeltaBytesSent", totalSnapshotDeltaBytesSent_.load()},
                          {"rejectedMessages", totalRejectedMessages_.load()},
                          {"rateLimitRejects", totalRateLimitRejects_.load()},
                          {"rejectedConnections", totalRejectedConnections_.load()},
                          {"protocolViolations", totalProtocolViolations_.load()},
                          {"sendFailures", totalSendFailures_.load()},
                      }}};
  }

  nlohmann::json GameServer::roomsJson() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    struct RoomCounts
    {
      int players{0};
      int humans{0};
      int bots{0};
    };

    std::unordered_map<std::string, RoomCounts> rooms;
    for (const auto &[_, player] : players_)
    {
      auto &room = rooms[player.roomCode.empty() ? "public" : player.roomCode];
      ++room.players;
      if (player.bot)
      {
        ++room.bots;
      }
      else
      {
        ++room.humans;
      }
    }

    RoomCounts privateCounts;
    std::size_t privateRooms = 0;
    nlohmann::json result = {
        {"service", "vix-arena"},
        {"updatedAt", isoTimestampUtc()},
        {"activeRooms", rooms.size()},
        {"listedRooms", 0},
        {"hiddenRooms", 0},
        {"rooms", nlohmann::json::array()},
    };
    for (const auto &[code, room] : rooms)
    {
      if (code == "public")
      {
        result["rooms"].push_back({
            {"code", "public"},
            {"listed", true},
            {"players", room.players},
            {"humans", room.humans},
            {"bots", room.bots},
        });
        continue;
      }

      ++privateRooms;
      privateCounts.players += room.players;
      privateCounts.humans += room.humans;
      privateCounts.bots += room.bots;
    }

    if (privateRooms > 0)
    {
      result["rooms"].push_back({
          {"code", nullptr},
          {"label", privateRooms == 1 ? "private room" : "private rooms"},
          {"listed", false},
          {"roomCount", privateRooms},
          {"players", privateCounts.players},
          {"humans", privateCounts.humans},
          {"bots", privateCounts.bots},
      });
    }
    result["listedRooms"] = std::count_if(result["rooms"].begin(), result["rooms"].end(), [](const nlohmann::json &room)
                                          { return room.value("listed", false); });
    result["hiddenRooms"] = privateRooms;
    std::sort(result["rooms"].begin(), result["rooms"].end(), [](const nlohmann::json &left, const nlohmann::json &right)
              {
                if (left.value("listed", false) != right.value("listed", false))
                  return left.value("listed", false);
                const std::string leftCode = left.contains("code") && left.at("code").is_string() ? left.at("code").get<std::string>() : "";
                const std::string rightCode = right.contains("code") && right.at("code").is_string() ? right.at("code").get<std::string>() : "";
                return leftCode < rightCode;
              });
    return result;
  }

  nlohmann::json GameServer::leaderboardJson(const std::string &roomCode) const
  {
    const std::string room = roomCode.empty() ? "" : sanitizeRoomCode(roomCode);
    if (persistence_ && persistence_->enabled())
    {
      try
      {
        return persistence_->leaderboardJson(10, room);
      }
      catch (const std::exception &e)
      {
        std::cerr << "PostgreSQL leaderboard read failed, falling back to JSON state: " << e.what() << '\n';
      }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    return leaderboardJsonLocked(10, room);
  }

  nlohmann::json GameServer::matchesJson(const std::string &roomCode) const
  {
    const std::string room = roomCode.empty() ? "" : sanitizeRoomCode(roomCode);
    if (persistence_ && persistence_->enabled())
    {
      try
      {
        return persistence_->matchesJson(20, room);
      }
      catch (const std::exception &e)
      {
        std::cerr << "PostgreSQL matches read failed, falling back to JSON state: " << e.what() << '\n';
      }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    return matchesJsonLocked(20, room);
  }

  std::string GameServer::metricsText() const
  {
    std::vector<std::uint64_t> tickSamples;
    std::size_t connectedPlayers = 0;
    std::size_t humanPlayers = 0;
    std::size_t botPlayers = 0;
    std::uint64_t uptime = 0;
    std::uint64_t totalTicks = 0;
    std::uint64_t maxTickDurationUs = 0;
    std::uint64_t totalConnections = 0;
    std::uint64_t totalChatMessages = 0;
    std::uint64_t totalOrbPickups = 0;
    std::uint64_t totalPowerups = 0;
    std::uint64_t totalQuests = 0;
    std::uint64_t totalRounds = 0;
    std::uint64_t totalControlPoints = 0;
    std::size_t leaderboardEntries = 0;
    std::size_t matchHistorySize = 0;
    std::size_t activeWsConnections = 0;
    std::size_t activeRemoteAddresses = 0;
    PersistenceStatus persistenceStatus = persistence_ ? persistence_->status() : PersistenceStatus{};

    {
      std::lock_guard<std::mutex> lock(mutex_);
      connectedPlayers = players_.size();
      humanPlayers = humanCountLocked();
      botPlayers = botCountLocked();
      uptime = static_cast<std::uint64_t>(uptimeSeconds(startedAt_));
      totalTicks = totalTicks_;
      maxTickDurationUs = maxTickDurationUs_;
      totalConnections = totalConnections_;
      totalChatMessages = totalChatMessages_;
      totalOrbPickups = totalOrbPickups_;
      totalPowerups = totalPowerupsSinceStart_;
      totalQuests = totalQuestsCompleted_;
      totalRounds = totalRoundsCompleted_;
      totalControlPoints = totalControlZonePoints_;
      leaderboardEntries = leaderboard_.size();
      matchHistorySize = matchHistory_.size();
      activeWsConnections = sessionAbuse_.size();
      activeRemoteAddresses = connectionsByIp_.size();
      tickSamples.assign(recentTickDurationsUs_.begin(), recentTickDurationsUs_.end());
    }

    std::ostringstream out;
    appendMetric(out, "vix_arena_up", "Service health, 1 when process is responding.", "gauge", std::uint64_t{1});
    appendMetric(out, "vix_arena_uptime_seconds", "Seconds since process start.", "counter", uptime);
    appendMetric(out, "vix_arena_players_connected", "Current connected players including bots.", "gauge", static_cast<std::uint64_t>(connectedPlayers));
    appendMetric(out, "vix_arena_players_human", "Current connected human players.", "gauge", static_cast<std::uint64_t>(humanPlayers));
    appendMetric(out, "vix_arena_players_bot", "Current connected bot players.", "gauge", static_cast<std::uint64_t>(botPlayers));
    appendMetric(out, "vix_arena_connections_total", "Total WebSocket connections opened.", "counter", totalConnections);
    appendMetric(out, "vix_arena_ws_active_connections", "Current accepted WebSocket connections.", "gauge", static_cast<std::uint64_t>(activeWsConnections));
    appendMetric(out, "vix_arena_ws_active_remote_addresses", "Current distinct remote addresses with accepted WebSocket connections.", "gauge", static_cast<std::uint64_t>(activeRemoteAddresses));
    appendMetric(out, "vix_arena_ws_rejected_connections_total", "Total WebSocket connections rejected before entering the arena.", "counter", totalRejectedConnections_.load());
    appendMetric(out, "vix_arena_ws_messages_received_total", "Total WebSocket messages received.", "counter", totalMessagesReceived_.load());
    appendMetric(out, "vix_arena_ws_message_bytes_received_total", "Total WebSocket message bytes received.", "counter", totalMessageBytesReceived_.load());
    appendMetric(out, "vix_arena_ws_messages_sent_total", "Total WebSocket messages sent.", "counter", totalMessagesSent_.load());
    appendMetric(out, "vix_arena_ws_message_bytes_sent_total", "Total WebSocket message bytes sent.", "counter", totalMessageBytesSent_.load());
    appendMetric(out, "vix_arena_ws_snapshots_sent_total", "Total WebSocket snapshots sent.", "counter", totalSnapshotsSent_.load());
    appendMetric(out, "vix_arena_ws_snapshot_bytes_sent_total", "Total WebSocket snapshot bytes sent.", "counter", totalSnapshotBytesSent_.load());
    appendMetric(out, "vix_arena_ws_snapshot_deltas_sent_total", "Total WebSocket snapshot deltas sent.", "counter", totalSnapshotDeltasSent_.load());
    appendMetric(out, "vix_arena_ws_snapshot_delta_bytes_sent_total", "Total WebSocket snapshot delta bytes sent.", "counter", totalSnapshotDeltaBytesSent_.load());
    appendMetric(out, "vix_arena_ws_rejected_messages_total", "Total rejected WebSocket messages that returned an error.", "counter", totalRejectedMessages_.load());
    appendMetric(out, "vix_arena_ws_protocol_violations_total", "Total invalid WebSocket protocol messages.", "counter", totalProtocolViolations_.load());
    appendMetric(out, "vix_arena_rate_limit_rejections_total", "Total input or chat messages rejected by rate limits.", "counter", totalRateLimitRejects_.load());
    appendMetric(out, "vix_arena_send_failures_total", "Total WebSocket send failures.", "counter", totalSendFailures_.load());
    appendMetric(out, "vix_arena_ticks_total", "Total authoritative game loop ticks.", "counter", totalTicks);
    appendMetric(out, "vix_arena_tick_duration_microseconds_p50", "Recent tick duration p50 in microseconds.", "gauge", percentile(tickSamples, 50.0));
    appendMetric(out, "vix_arena_tick_duration_microseconds_p95", "Recent tick duration p95 in microseconds.", "gauge", percentile(tickSamples, 95.0));
    appendMetric(out, "vix_arena_tick_duration_microseconds_p99", "Recent tick duration p99 in microseconds.", "gauge", percentile(tickSamples, 99.0));
    appendMetric(out, "vix_arena_tick_duration_microseconds_max", "Maximum tick duration observed in microseconds.", "gauge", maxTickDurationUs);
    appendMetric(out, "vix_arena_chat_messages_total", "Total accepted chat messages.", "counter", totalChatMessages);
    appendMetric(out, "vix_arena_orb_pickups_total", "Total orb pickups.", "counter", totalOrbPickups);
    appendMetric(out, "vix_arena_powerup_pickups_total", "Total powerup pickups.", "counter", totalPowerups);
    appendMetric(out, "vix_arena_quests_completed_total", "Total Orb Run quests completed.", "counter", totalQuests);
    appendMetric(out, "vix_arena_rounds_completed_total", "Total rounds completed.", "counter", totalRounds);
    appendMetric(out, "vix_arena_control_zone_points_total", "Total points awarded by control zone.", "counter", totalControlPoints);
    appendMetric(out, "vix_arena_leaderboard_entries", "Current persistent leaderboard entries.", "gauge", static_cast<std::uint64_t>(leaderboardEntries));
    appendMetric(out, "vix_arena_match_history_entries", "Current persistent match history entries.", "gauge", static_cast<std::uint64_t>(matchHistorySize));
    appendMetric(out, "vix_arena_postgres_configured", "PostgreSQL persistence is configured.", "gauge", persistenceStatus.configured ? 1 : 0);
    appendMetric(out, "vix_arena_postgres_enabled", "PostgreSQL persistence is enabled.", "gauge", persistenceStatus.enabled ? 1 : 0);
    appendMetric(out, "vix_arena_postgres_queued_writes", "Queued PostgreSQL match writes.", "gauge", persistenceStatus.queuedWrites);
    appendMetric(out, "vix_arena_postgres_saved_matches_total", "Matches saved to PostgreSQL.", "counter", persistenceStatus.savedMatches);
    appendMetric(out, "vix_arena_postgres_failed_writes_total", "Failed PostgreSQL writes.", "counter", persistenceStatus.failedWrites);
    appendMetric(out, "vix_arena_postgres_schema_version", "Current PostgreSQL schema migration version.", "gauge", persistenceStatus.schemaVersion);

    return out.str();
  }
}
