#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "Player.hpp"
#include "World.hpp"

namespace arena
{
  [[nodiscard]] nlohmann::json playerToJson(const Player &player);
  [[nodiscard]] nlohmann::json errorMessage(const std::string &message);
  [[nodiscard]] nlohmann::json worldSummary(const World &world);
}
