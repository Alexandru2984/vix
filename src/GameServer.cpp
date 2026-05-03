#include "GameServer.hpp"

#include <cmath>
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
  }

  GameServer::GameServer()
      : rng_(std::random_device{}()),
        startedAt_(std::chrono::steady_clock::now())
  {
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
        player.lastChat = player.lastSeen - std::chrono::seconds(10);

        const std::string id = player.id;
        const std::string playerName = player.name;
        sessionToPlayer_[session] = id;
        players_[id] = std::move(player);

        welcome = {{"type", "welcome"}, {"id", id}, {"world", worldSummary(world_)}};
        joined = {{"type", "player_joined"}, {"id", id}, {"name", playerName}};
        snapshot = snapshotLocked();
        history.assign(chatHistory_.begin(), chatHistory_.end());
        newPlayer = true;
      }
    }

    if (!welcome.empty())
    {
      send(session, welcome);
      if (!history.empty())
      {
        send(session, {{"type", "chat_history"}, {"messages", history}});
      }
      send(session, snapshot);
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

    pit->second.input.up = jsonBool(message, "up");
    pit->second.input.down = jsonBool(message, "down");
    pit->second.input.left = jsonBool(message, "left");
    pit->second.input.right = jsonBool(message, "right");
    if (message.contains("seq") && message["seq"].is_number_unsigned())
    {
      pit->second.input.seq = message["seq"].get<std::uint64_t>();
    }
    pit->second.lastSeen = std::chrono::steady_clock::now();
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
    nlohmann::json pong = {{"type", "pong"}};
    if (message.contains("t"))
    {
      pong["t"] = message["t"];
    }
    send(session, pong);
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
      }

      const double nextX = player.x + dx * playerSpeed_ * dt;
      const double nextY = player.y + dy * playerSpeed_ * dt;

      double candidateX = nextX;
      double candidateY = player.y;
      world_.clampToBounds(candidateX, candidateY);
      if (!world_.collides(candidateX, candidateY))
      {
        player.x = candidateX;
      }

      candidateX = player.x;
      candidateY = nextY;
      world_.clampToBounds(candidateX, candidateY);
      if (!world_.collides(candidateX, candidateY))
      {
        player.y = candidateY;
      }
    }
  }

  void GameServer::cleanupStaleLocked(std::vector<nlohmann::json> &leftEvents)
  {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = players_.begin(); it != players_.end();)
    {
      const bool expiredSession = it->second.session.expired();
      const bool stale = now - it->second.lastSeen > std::chrono::minutes(5);
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
        {"serverTime", isoTimestampUtc()}};
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
        {"world", worldSummary(world_)}};
  }

  nlohmann::json GameServer::statsJson() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        {"service", "vix-arena"},
        {"connectedPlayers", players_.size()},
        {"uptimeSeconds", uptimeSeconds(startedAt_)},
        {"tickRateTarget", tickRateTarget_},
        {"totalConnectionsSinceStart", totalConnections_},
        {"totalChatMessagesSinceStart", totalChatMessages_}};
  }
}
