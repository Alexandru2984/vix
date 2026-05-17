#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace arena
{
  struct MatchParticipantRecord
  {
    std::string id;
    std::string name;
    bool bot{false};
    int score{0};
    int orbPickups{0};
    int powerups{0};
    int quests{0};
    int controlZonePoints{0};
    int abilitiesUsed{0};
    bool winner{false};
  };

  struct MatchRecord
  {
    std::string roomCode{"public"};
    std::uint64_t round{0};
    std::string endedAt;
    std::string winnerId;
    std::string winnerName;
    int winnerScore{0};
    int durationSeconds{0};
    std::vector<MatchParticipantRecord> participants;
  };

  struct PersistenceStatus
  {
    bool configured{false};
    bool enabled{false};
    std::uint64_t queuedWrites{0};
    std::uint64_t savedMatches{0};
    std::uint64_t failedWrites{0};
    std::uint64_t schemaVersion{0};
    std::string lastError;
  };

  class PersistenceStore
  {
  public:
    explicit PersistenceStore(std::string databaseUrl, std::filesystem::path migrationsDir = {}, std::size_t matchHistoryLimit = 50);
    ~PersistenceStore();

    PersistenceStore(const PersistenceStore &) = delete;
    PersistenceStore &operator=(const PersistenceStore &) = delete;

    [[nodiscard]] PersistenceStatus status() const;
    [[nodiscard]] bool enabled() const noexcept { return enabled_.load(); }
    void enqueueMatch(MatchRecord record);
    [[nodiscard]] nlohmann::json leaderboardJson(std::size_t limit = 10) const;
    [[nodiscard]] nlohmann::json matchesJson(std::size_t limit = 20) const;

  private:
    void migrate();
    void workerLoop();
    void saveMatch(const MatchRecord &record);
    [[nodiscard]] nlohmann::json participantsJson(const MatchRecord &record) const;
    void setLastError(std::string message) const;

    std::string databaseUrl_;
    std::filesystem::path migrationsDir_;
    std::size_t matchHistoryLimit_{50};
    std::atomic<bool> configured_{false};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> stopping_{false};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<MatchRecord> queue_;
    std::thread worker_;
    std::atomic<std::uint64_t> savedMatches_{0};
    std::atomic<std::uint64_t> failedWrites_{0};
    std::atomic<std::uint64_t> schemaVersion_{0};
    mutable std::string lastError_;
  };
}
