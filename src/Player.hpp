#pragma once

#include <chrono>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace arena
{
  struct ClientConnection : public std::enable_shared_from_this<ClientConnection>
  {
    using SendFn = std::function<void(const std::string &)>;
    using CloseFn = std::function<void(const std::string &)>;
    SendFn send;
    CloseFn close;
    std::atomic<bool> open{true};
    std::string remoteAddress;
    std::string roomCode{"public"};
  };

  struct PlayerInput
  {
    bool up{false};
    bool down{false};
    bool left{false};
    bool right{false};
    std::uint64_t seq{0};
  };

  struct Player
  {
    std::string id;
    std::string name;
    std::string color;
    std::string roomCode{"public"};
    bool bot{false};
    int score{0};
    int orbQuestProgress{0};
    int roundOrbPickups{0};
    int roundPowerups{0};
    int roundQuests{0};
    int roundControlZonePoints{0};
    int roundAbilitiesUsed{0};
    double controlCarry{0.0};
    double x{0.0};
    double y{0.0};
    PlayerInput input;
    std::weak_ptr<ClientConnection> session;
    std::chrono::steady_clock::time_point lastSeen;
    std::chrono::steady_clock::time_point lastInput;
    std::chrono::steady_clock::time_point lastChat;
    std::chrono::steady_clock::time_point speedBoostUntil;
    std::chrono::steady_clock::time_point dashReadyAt;
    std::chrono::steady_clock::time_point shieldUntil;
    std::chrono::steady_clock::time_point shieldReadyAt;
    std::chrono::steady_clock::time_point magnetUntil;
    std::chrono::steady_clock::time_point magnetReadyAt;
    std::chrono::steady_clock::time_point nextBotDecisionAt;
    double facingX{1.0};
    double facingY{0.0};
    double botTargetX{0.0};
    double botTargetY{0.0};
  };
}
