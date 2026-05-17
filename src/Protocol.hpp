#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "Player.hpp"
#include "World.hpp"

namespace arena
{
  inline constexpr int protocolVersion = 2;
  inline constexpr std::string_view protocolFeatureSnapshotDelta = "snapshot_delta";

  [[nodiscard]] nlohmann::json playerToJson(const Player &player);
  [[nodiscard]] nlohmann::json errorMessage(const std::string &message);
  [[nodiscard]] nlohmann::json worldSummary(const World &world);
}
