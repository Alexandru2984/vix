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
    SendFn send;
    std::atomic<bool> open{true};
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
    double x{0.0};
    double y{0.0};
    PlayerInput input;
    std::weak_ptr<ClientConnection> session;
    std::chrono::steady_clock::time_point lastSeen;
    std::chrono::steady_clock::time_point lastChat;
  };
}
