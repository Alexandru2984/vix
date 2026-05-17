#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <exception>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "GameServer.hpp"
#include "Protocol.hpp"
#include "Utils.hpp"
#include "Validation.hpp"
#include "World.hpp"

namespace
{
  struct TestFailure : std::runtime_error
  {
    using std::runtime_error::runtime_error;
  };

  int testsRun = 0;

  void require(bool condition, std::string_view message)
  {
    if (!condition)
    {
      throw TestFailure(std::string(message));
    }
  }

  template <typename L, typename R>
  void requireEq(const L &left, const R &right, std::string_view message)
  {
    if (!(left == right))
    {
      throw TestFailure(std::string(message));
    }
  }

  void run(std::string_view name, void (*fn)())
  {
    ++testsRun;
    try
    {
      fn();
      std::cout << "ok - " << name << '\n';
    }
    catch (const std::exception &e)
    {
      std::cerr << "not ok - " << name << ": " << e.what() << '\n';
      throw;
    }
  }

  struct CapturedClient
  {
    std::shared_ptr<arena::ClientConnection> connection{std::make_shared<arena::ClientConnection>()};
    std::vector<nlohmann::json> messages;

    CapturedClient()
    {
      connection->send = [this](const std::string &payload)
      {
        messages.push_back(nlohmann::json::parse(payload));
      };
    }

    [[nodiscard]] bool sawType(const std::string &type) const
    {
      return std::any_of(messages.begin(), messages.end(), [&](const nlohmann::json &message)
                         { return message.value("type", "") == type; });
    }

    [[nodiscard]] const nlohmann::json *firstType(const std::string &type) const
    {
      const auto it = std::find_if(messages.begin(), messages.end(), [&](const nlohmann::json &message)
                                   { return message.value("type", "") == type; });
      return it == messages.end() ? nullptr : &*it;
    }
  };

  void validationCleansAndCapsText()
  {
    requireEq(arena::sanitizeDisplayName("  Alice\t\nBob  "), std::string("Alice Bob"), "display name whitespace should collapse");
    requireEq(arena::sanitizeChatMessage("hi\x01\x02 there"), std::string("hi there"), "control characters should be replaced by one space");
    requireEq(arena::sanitizeDisplayName(std::string(80, 'a')).size(), arena::maxNameLength, "display name should be capped");
    requireEq(arena::sanitizeChatMessage(std::string(400, 'b')).size(), arena::maxChatLength, "chat message should be capped");
  }

  void worldBoundsCollisionsAndSpawnsAreStable()
  {
    arena::World world;
    requireEq(world.width(), 2000.0, "world width");
    requireEq(world.height(), 1200.0, "world height");
    require(!world.obstacles().empty(), "world should expose static obstacles");
    require(world.collides(1.0, 1.0), "near origin should collide with bounds");
    require(world.collides(430.0, 270.0), "known obstacle should collide");
    require(!world.collides(1000.0, 100.0), "known open point should not collide");

    double x = -100.0;
    double y = 3000.0;
    world.clampToBounds(x, y);
    requireEq(x, arena::World::playerRadius, "clamp x lower bound");
    requireEq(y, world.height() - arena::World::playerRadius, "clamp y upper bound");

    std::mt19937 rng(123);
    for (int i = 0; i < 50; ++i)
    {
      const auto [sx, sy] = world.randomSpawn(rng);
      require(!world.collides(sx, sy), "random spawn should avoid blocked space");
    }
  }

  void protocolSerializesPlayerSafely()
  {
    arena::Player player;
    const auto now = std::chrono::steady_clock::now();
    player.id = "p-7";
    player.name = "Micu";
    player.color = "#66ccff";
    player.x = 12.5;
    player.y = 44.0;
    player.score = 42;
    player.orbQuestProgress = 2;
    player.speedBoostUntil = now + std::chrono::milliseconds(1500);
    player.dashReadyAt = now;
    player.shieldUntil = now;
    player.shieldReadyAt = now + std::chrono::milliseconds(2500);
    player.magnetUntil = now;
    player.magnetReadyAt = now + std::chrono::milliseconds(3500);

    const auto json = arena::playerToJson(player);
    requireEq(json.value("id", ""), std::string("p-7"), "player id should serialize");
    requireEq(json.value("score", 0), 42, "player score should serialize");
    requireEq(json.at("quest").value("progress", 0), 2, "quest progress should serialize");
    require(json.at("boostMs").get<std::int64_t>() > 0, "boost should be active");
    require(json.at("abilities").at("shieldCooldownMs").get<std::int64_t>() > 0, "shield cooldown should be positive");
    requireEq(arena::errorMessage("bad").dump(), std::string("{\"message\":\"bad\",\"type\":\"error\"}"), "error shape should stay stable");
    requireEq(arena::makePlayerId(9), std::string("p-9"), "player id helper");
    requireEq(arena::makeGuestName(9), std::string("Guest 1009"), "guest helper");
  }

  void gameServerRejectsBadPayloads()
  {
    arena::GameServer server;
    CapturedClient client;
    server.onOpen(client.connection);

    server.onMessage(client.connection.get(), "{");
    require(client.sawType("error"), "invalid JSON should return error");
    requireEq(client.messages.back().value("message", ""), std::string("invalid JSON object"), "invalid JSON error message");

    server.onMessage(client.connection.get(), std::string(arena::maxWsPayloadBytes + 1, 'x'));
    requireEq(client.messages.back().value("message", ""), std::string("message too large"), "oversized payload should be rejected");
  }

  void gameServerJoinChatAndRateLimitFlow()
  {
    arena::GameServer server;
    CapturedClient client;
    server.onOpen(client.connection);

    server.onMessage(client.connection.get(), R"({"type":"join","name":"  Test Pilot  "})");
    const auto *welcome = client.firstType("welcome");
    require(welcome != nullptr, "join should send welcome");
    requireEq(welcome->value("id", ""), std::string("p-1"), "first player id should be stable");
    require(client.sawType("snapshot"), "join should send snapshot");

    const auto health = server.healthJson();
    requireEq(health.value("humans", 0), 1, "one human should be joined");
    requireEq(health.value("bots", 0), 3, "bots should fill solo arena");

    server.onMessage(client.connection.get(), R"({"type":"chat","message":"hello arena"})");
    requireEq(client.messages.back().value("type", ""), std::string("chat"), "accepted chat should broadcast");
    requireEq(client.messages.back().value("message", ""), std::string("hello arena"), "chat text should be sanitized and broadcast");

    server.onMessage(client.connection.get(), R"({"type":"chat","message":"too soon"})");
    requireEq(client.messages.back().value("type", ""), std::string("error"), "rapid chat should be rate limited");
    requireEq(client.messages.back().value("message", ""), std::string("chat rate limit"), "chat rate limit message");

    const auto stats = server.statsJson();
    requireEq(stats.value("totalConnectionsSinceStart", 0), 1, "connection counter");
    requireEq(stats.value("totalChatMessagesSinceStart", 0), 1, "accepted chat counter");

    server.onClose(client.connection.get());
    const auto afterClose = server.healthJson();
    requireEq(afterClose.value("humans", 0), 0, "close should remove human");
    requireEq(afterClose.value("bots", 0), 0, "bots should leave when no humans remain");
  }

  void gameServerExposesStatsAndMetrics()
  {
    arena::GameServer server;
    CapturedClient client;
    server.onOpen(client.connection);
    server.onMessage(client.connection.get(), R"({"type":"join","name":"Metrics"})");
    server.onMessage(client.connection.get(), R"({"type":"chat","message":"hello metrics"})");
    server.onMessage(client.connection.get(), R"({"type":"chat","message":"rate limited"})");

    const auto stats = server.statsJson();
    require(stats.contains("websocket"), "stats should expose websocket counters");
    require(stats.contains("tickDurationUs"), "stats should expose tick durations");
    require(stats.at("websocket").value("messagesReceived", 0) >= 3, "stats should count received messages");
    require(stats.at("websocket").value("rejectedMessages", 0) >= 1, "stats should count rejected messages");
    require(stats.at("websocket").value("rateLimitRejects", 0) >= 1, "stats should count rate limit rejects");

    const std::string metrics = server.metricsText();
    require(metrics.find("# HELP vix_arena_up") != std::string::npos, "metrics should include HELP lines");
    require(metrics.find("vix_arena_up 1") != std::string::npos, "metrics should include up gauge");
    require(metrics.find("vix_arena_ws_messages_received_total") != std::string::npos, "metrics should include websocket counters");
    require(metrics.find("vix_arena_tick_duration_microseconds_p95") != std::string::npos, "metrics should include tick duration gauges");
  }
}

int main()
{
  try
  {
    run("validation cleans and caps text", validationCleansAndCapsText);
    run("world bounds, collisions, and spawns are stable", worldBoundsCollisionsAndSpawnsAreStable);
    run("protocol serializes player safely", protocolSerializesPlayerSafely);
    run("game server rejects bad payloads", gameServerRejectsBadPayloads);
    run("game server join, chat, and rate limit flow", gameServerJoinChatAndRateLimitFlow);
    run("game server exposes stats and metrics", gameServerExposesStatsAndMetrics);
  }
  catch (const std::exception &)
  {
    return 1;
  }

  std::cout << testsRun << " tests passed\n";
  return 0;
}
