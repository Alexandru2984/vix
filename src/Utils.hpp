#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace arena
{
  [[nodiscard]] std::string isoTimestampUtc();
  [[nodiscard]] std::int64_t uptimeSeconds(std::chrono::steady_clock::time_point startedAt);
  [[nodiscard]] std::string makePlayerId(std::uint64_t n);
  [[nodiscard]] std::string makeGuestName(std::uint64_t n);
}
