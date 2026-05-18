#pragma once

#include <string>

namespace arena
{
  constexpr std::size_t maxNameLength = 18;
  constexpr std::size_t maxChatLength = 220;
  constexpr std::size_t maxWsPayloadBytes = 4096;

  [[nodiscard]] std::string sanitizeDisplayName(const std::string &input);
  [[nodiscard]] std::string sanitizeChatMessage(const std::string &input);
  [[nodiscard]] std::string sanitizeRoomCode(const std::string &input);
  [[nodiscard]] bool isTruthyBoolJsonCompatible(bool value);
}
