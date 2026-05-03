#include "Protocol.hpp"

namespace arena
{
  nlohmann::json playerToJson(const Player &player)
  {
    return {
        {"id", player.id},
        {"name", player.name},
        {"x", player.x},
        {"y", player.y},
        {"color", player.color}};
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
