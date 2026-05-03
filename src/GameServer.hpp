#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "Player.hpp"
#include "World.hpp"

namespace arena
{
  class GameServer
  {
  public:
    GameServer();
    ~GameServer();

    GameServer(const GameServer &) = delete;
    GameServer &operator=(const GameServer &) = delete;

    void start();
    void stop();

    void onOpen(const std::shared_ptr<ClientConnection> &session);
    void onClose(ClientConnection *session);
    void onMessage(ClientConnection *session, const std::string &payload);

    [[nodiscard]] nlohmann::json healthJson() const;
    [[nodiscard]] nlohmann::json stateJson() const;
    [[nodiscard]] nlohmann::json statsJson() const;

    [[nodiscard]] int tickRateTarget() const noexcept { return tickRateTarget_; }

  private:
    using SessionPtr = std::shared_ptr<ClientConnection>;

    void handleJoin(ClientConnection *session, const nlohmann::json &message);
    void handleInput(ClientConnection *session, const nlohmann::json &message);
    void handleChat(ClientConnection *session, const nlohmann::json &message);
    void handlePing(ClientConnection *session, const nlohmann::json &message);

    void tickLoop();
    void step(double dt);
    void cleanupStaleLocked(std::vector<nlohmann::json> &leftEvents);
    [[nodiscard]] nlohmann::json snapshotLocked() const;
    [[nodiscard]] std::vector<SessionPtr> liveSessionsLocked() const;

    void send(ClientConnection *session, const nlohmann::json &message);
    void broadcast(const nlohmann::json &message);
    void broadcastTo(const std::vector<SessionPtr> &sessions, const nlohmann::json &message);

    [[nodiscard]] std::string randomColor();

    mutable std::mutex mutex_;
    World world_;
    std::unordered_map<std::string, Player> players_;
    std::unordered_map<ClientConnection *, std::string> sessionToPlayer_;
    std::deque<nlohmann::json> chatHistory_;
    std::mt19937 rng_;

    std::atomic<bool> running_{false};
    std::thread tickThread_;
    std::chrono::steady_clock::time_point startedAt_;
    std::uint64_t nextPlayerNumber_{1};
    std::uint64_t totalConnections_{0};
    std::uint64_t totalChatMessages_{0};

    static constexpr int tickRateTarget_{20};
    static constexpr double playerSpeed_{235.0};
  };
}
