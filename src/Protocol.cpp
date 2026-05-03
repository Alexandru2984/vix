#include "Protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace arena
{
  nlohmann::json playerToJson(const Player &player)
  {
    const auto now = std::chrono::steady_clock::now();
    const auto boostMs = std::max<std::int64_t>(
        0,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            player.speedBoostUntil - now)
            .count());

    return {
        {"id", player.id},
        {"name", player.name},
        {"x", player.x},
        {"y", player.y},
        {"color", player.color},
        {"score", player.score},
        {"quest", {
                      {"name", "Orb Run"},
                      {"progress", player.orbQuestProgress},
                      {"goal", 3},
                      {"reward", 20},
                  }},
        {"boostMs", boostMs},
        {"abilities", {
                          {"dashCooldownMs", std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(player.dashReadyAt - now).count())},
                          {"shieldMs", std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(player.shieldUntil - now).count())},
                          {"shieldCooldownMs", std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(player.shieldReadyAt - now).count())},
                          {"magnetMs", std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(player.magnetUntil - now).count())},
                          {"magnetCooldownMs", std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(player.magnetReadyAt - now).count())},
                      }}};
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
