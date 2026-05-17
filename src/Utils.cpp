#include "Utils.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace arena
{
  std::string isoTimestampUtc()
  {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
  }

  std::int64_t unixTimeMs()
  {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  std::int64_t uptimeSeconds(std::chrono::steady_clock::time_point startedAt)
  {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - startedAt)
        .count();
  }

  std::string makePlayerId(std::uint64_t n)
  {
    return "p-" + std::to_string(n);
  }

  std::string makeGuestName(std::uint64_t n)
  {
    return "Guest " + std::to_string(1000 + (n % 9000));
  }
}
