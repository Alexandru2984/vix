#include "Persistence.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>

#include <pqxx/pqxx>

#include "Utils.hpp"

namespace arena
{
  namespace
  {
    constexpr std::size_t maxQueuedMatches = 128;

    const char *schemaSql = R"sql(
CREATE TABLE IF NOT EXISTS vix_matches (
  id BIGSERIAL PRIMARY KEY,
  round_number BIGINT NOT NULL,
  ended_at TIMESTAMPTZ NOT NULL,
  winner_id TEXT NOT NULL,
  winner_name TEXT NOT NULL,
  winner_score INTEGER NOT NULL,
  duration_seconds INTEGER NOT NULL,
  human_players INTEGER NOT NULL,
  bot_players INTEGER NOT NULL,
  total_players INTEGER NOT NULL,
  participant_count INTEGER NOT NULL,
  participants JSONB NOT NULL DEFAULT '[]'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS vix_match_players (
  id BIGSERIAL PRIMARY KEY,
  match_id BIGINT NOT NULL REFERENCES vix_matches(id) ON DELETE CASCADE,
  round_number BIGINT NOT NULL,
  ended_at TIMESTAMPTZ NOT NULL,
  player_id TEXT NOT NULL,
  name TEXT NOT NULL,
  is_bot BOOLEAN NOT NULL,
  is_winner BOOLEAN NOT NULL,
  score INTEGER NOT NULL,
  orb_pickups INTEGER NOT NULL,
  powerups INTEGER NOT NULL,
  quests INTEGER NOT NULL,
  control_zone_points INTEGER NOT NULL,
  abilities_used INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_vix_matches_ended_at ON vix_matches (ended_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_vix_match_players_leaderboard
  ON vix_match_players (is_bot, name, is_winner, score, ended_at);
)sql";

    std::string limitString(std::size_t limit, std::size_t fallback)
    {
      const std::size_t clamped = std::clamp(limit == 0 ? fallback : limit, std::size_t{1}, std::size_t{100});
      return std::to_string(clamped);
    }
  }

  PersistenceStore::PersistenceStore(std::string databaseUrl, std::size_t matchHistoryLimit)
      : databaseUrl_(std::move(databaseUrl)),
        matchHistoryLimit_(matchHistoryLimit)
  {
    configured_ = !databaseUrl_.empty();
    if (!configured_.load())
    {
      return;
    }

    try
    {
      migrate();
      enabled_ = true;
      worker_ = std::thread(&PersistenceStore::workerLoop, this);
      std::cout << "PostgreSQL persistence enabled\n";
    }
    catch (const std::exception &e)
    {
      enabled_ = false;
      setLastError(e.what());
      std::cerr << "PostgreSQL persistence disabled: " << e.what() << '\n';
    }
  }

  PersistenceStore::~PersistenceStore()
  {
    stopping_ = true;
    cv_.notify_all();
    if (worker_.joinable())
    {
      worker_.join();
    }
  }

  PersistenceStatus PersistenceStore::status() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        configured_.load(),
        enabled_.load(),
        static_cast<std::uint64_t>(queue_.size()),
        savedMatches_.load(),
        failedWrites_.load(),
        lastError_};
  }

  void PersistenceStore::enqueueMatch(MatchRecord record)
  {
    if (!enabled_.load())
    {
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (queue_.size() >= maxQueuedMatches)
      {
        ++failedWrites_;
        lastError_ = "persistence queue full";
        return;
      }
      queue_.push_back(std::move(record));
    }
    cv_.notify_one();
  }

  nlohmann::json PersistenceStore::leaderboardJson(std::size_t limit) const
  {
    nlohmann::json result = {
        {"service", "vix-arena"},
        {"source", "postgresql"},
        {"updatedAt", isoTimestampUtc()},
        {"entries", nlohmann::json::array()},
    };

    try
    {
      pqxx::connection connection(databaseUrl_);
      pqxx::read_transaction tx(connection);
      const auto rows = tx.exec(
          R"sql(
SELECT
  name,
  count(*)::BIGINT AS rounds,
  count(*) FILTER (WHERE is_winner)::BIGINT AS wins,
  coalesce(sum(score), 0)::BIGINT AS total_score,
  coalesce(max(score), 0)::INTEGER AS best_score,
  coalesce(avg(score), 0)::DOUBLE PRECISION AS average_score,
  to_char(max(ended_at AT TIME ZONE 'UTC'), 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS last_played_at
FROM vix_match_players
WHERE NOT is_bot
GROUP BY name
ORDER BY wins DESC, best_score DESC, total_score DESC, name ASC
LIMIT $1
)sql",
          pqxx::params{limitString(limit, 10)});

      std::size_t rank = 1;
      for (const auto &row : rows)
      {
        result["entries"].push_back({
            {"rank", rank++},
            {"name", row["name"].c_str()},
            {"rounds", row["rounds"].as<std::uint64_t>()},
            {"wins", row["wins"].as<std::uint64_t>()},
            {"totalScore", row["total_score"].as<std::uint64_t>()},
            {"averageScore", row["average_score"].as<double>()},
            {"bestScore", row["best_score"].as<int>()},
            {"lastPlayedAt", row["last_played_at"].c_str()},
        });
      }
      tx.commit();
      result["updatedAt"] = isoTimestampUtc();
      return result;
    }
    catch (const std::exception &e)
    {
      setLastError(e.what());
      throw;
    }
  }

  nlohmann::json PersistenceStore::matchesJson(std::size_t limit) const
  {
    nlohmann::json result = {
        {"service", "vix-arena"},
        {"source", "postgresql"},
        {"updatedAt", ""},
        {"matches", nlohmann::json::array()},
    };

    try
    {
      pqxx::connection connection(databaseUrl_);
      pqxx::read_transaction tx(connection);
      const auto rows = tx.exec(
          R"sql(
SELECT
  round_number,
  to_char(ended_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS ended_at,
  winner_id,
  winner_name,
  winner_score,
  participants::TEXT AS participants
FROM vix_matches
ORDER BY ended_at DESC, id DESC
LIMIT $1
)sql",
          pqxx::params{limitString(limit, matchHistoryLimit_)});

      for (const auto &row : rows)
      {
        nlohmann::json participants = nlohmann::json::array();
        const auto parsed = nlohmann::json::parse(row["participants"].c_str(), nullptr, false);
        if (parsed.is_array())
        {
          participants = parsed;
        }

        const std::string endedAt = row["ended_at"].c_str();
        if (result["updatedAt"].get<std::string>().empty())
        {
          result["updatedAt"] = endedAt;
        }
        result["matches"].push_back({
            {"round", row["round_number"].as<std::uint64_t>()},
            {"endedAt", endedAt},
            {"winner", {
                           {"id", row["winner_id"].c_str()},
                           {"name", row["winner_name"].c_str()},
                           {"score", row["winner_score"].as<int>()},
                       }},
            {"participants", participants},
        });
      }
      tx.commit();
      return result;
    }
    catch (const std::exception &e)
    {
      setLastError(e.what());
      throw;
    }
  }

  void PersistenceStore::migrate()
  {
    pqxx::connection connection(databaseUrl_);
    pqxx::work tx(connection);
    tx.exec(schemaSql);
    tx.commit();
  }

  void PersistenceStore::workerLoop()
  {
    while (true)
    {
      MatchRecord record;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]
                 { return stopping_.load() || !queue_.empty(); });
        if (stopping_.load() && queue_.empty())
        {
          break;
        }
        record = std::move(queue_.front());
        queue_.pop_front();
      }

      try
      {
        saveMatch(record);
        ++savedMatches_;
      }
      catch (const std::exception &e)
      {
        ++failedWrites_;
        setLastError(e.what());
        std::cerr << "failed to persist match: " << e.what() << '\n';
      }
    }
  }

  void PersistenceStore::saveMatch(const MatchRecord &record)
  {
    int humanPlayers = 0;
    int botPlayers = 0;
    for (const auto &participant : record.participants)
    {
      participant.bot ? ++botPlayers : ++humanPlayers;
    }

    pqxx::connection connection(databaseUrl_);
    pqxx::work tx(connection);
    const auto inserted = tx.exec(
        R"sql(
INSERT INTO vix_matches (
  round_number, ended_at, winner_id, winner_name, winner_score, duration_seconds,
  human_players, bot_players, total_players, participant_count, participants
) VALUES ($1, $2::timestamptz, $3, $4, $5, $6, $7, $8, $9, $10, $11::jsonb)
RETURNING id
)sql",
        pqxx::params{
            record.round,
            record.endedAt,
            record.winnerId,
            record.winnerName,
            record.winnerScore,
            record.durationSeconds,
            humanPlayers,
            botPlayers,
            humanPlayers + botPlayers,
            static_cast<int>(record.participants.size()),
            participantsJson(record).dump()});

    if (inserted.empty())
    {
      throw std::runtime_error("match insert returned no id");
    }
    const auto matchId = inserted.front().front().as<std::uint64_t>();

    for (const auto &participant : record.participants)
    {
      tx.exec(
          R"sql(
INSERT INTO vix_match_players (
  match_id, round_number, ended_at, player_id, name, is_bot, is_winner,
  score, orb_pickups, powerups, quests, control_zone_points, abilities_used
) VALUES ($1, $2, $3::timestamptz, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13)
)sql",
          pqxx::params{
              matchId,
              record.round,
              record.endedAt,
              participant.id,
              participant.name,
              participant.bot,
              participant.winner,
              participant.score,
              participant.orbPickups,
              participant.powerups,
              participant.quests,
              participant.controlZonePoints,
              participant.abilitiesUsed});
    }

    tx.commit();
  }

  nlohmann::json PersistenceStore::participantsJson(const MatchRecord &record) const
  {
    nlohmann::json participants = nlohmann::json::array();
    for (const auto &participant : record.participants)
    {
      participants.push_back({
          {"id", participant.id},
          {"name", participant.name},
          {"score", participant.score},
          {"bot", participant.bot},
          {"winner", participant.winner},
          {"orbPickups", participant.orbPickups},
          {"powerups", participant.powerups},
          {"quests", participant.quests},
          {"controlZonePoints", participant.controlZonePoints},
          {"abilitiesUsed", participant.abilitiesUsed},
      });
    }
    return participants;
  }

  void PersistenceStore::setLastError(std::string message) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_ = std::move(message);
  }
}
