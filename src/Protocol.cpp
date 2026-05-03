#include "Protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace arena
{
  nlohmann::json playerToJson(const Player &player)
  {
    const auto boostMs = std::max<std::int64_t>(
        0,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            player.speedBoostUntil - std::chrono::steady_clock::now())
            .count());

    return {
        {"id", player.id},
        {"name", player.name},
        {"x", player.x},
        {"y", player.y},
        {"color", player.color},
        {"score", player.score},
        {"boostMs", boostMs}};
  }

  nlohmann::json errorMessage(const std::string &message)
  {
    return {{"type", "error"}, {"message", message}};
  }

  nlohmann::json worldSummary(const World &world)
  {
    return world.toJson();
  }
}
