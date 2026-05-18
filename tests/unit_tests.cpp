#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
    bool closed{false};

    CapturedClient()
    {
      connection->send = [this](const std::string &payload)
      {
        messages.push_back(nlohmann::json::parse(payload));
      };
      connection->close = [this](const std::string &)
      {
        closed = true;
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
    player.input.seq = 123;
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
    requireEq(json.value("inputSeq", 0), 123, "input sequence should serialize");
    requireEq(json.at("quest").value("progress", 0), 2, "quest progress should serialize");
    require(json.at("boostMs").get<std::int64_t>() > 0, "boost should be active");
    require(json.at("abilities").at("shieldCooldownMs").get<std::int64_t>() > 0, "shield cooldown should be positive");
    const auto error = arena::errorMessage("bad");
    requireEq(error.value("type", ""), std::string("error"), "error type should stay stable");
    requireEq(error.value("message", ""), std::string("bad"), "error message should stay stable");
    requireEq(error.value("protocolVersion", 0), arena::protocolVersion, "error should expose protocol version");
    requireEq(arena::makePlayerId(9), std::string("p-9"), "player id helper");
    requireEq(arena::makeGuestName(9), std::string("Guest 1009"), "guest helper");
    requireEq(arena::sanitizeRoomCode("  Duel Room!! "), std::string("duel-room"), "room code should normalize");
  }

  void gameServerRejectsBadPayloads()
  {
    arena::GameServer server;
    CapturedClient client;
    require(server.onOpen(client.connection), "client should open");

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
    require(server.onOpen(client.connection), "client should open");

    server.onMessage(client.connection.get(), R"({"type":"join","name":"  Test Pilot  ","protocolVersion":2,"supports":["snapshot_delta"]})");
    const auto *welcome = client.firstType("welcome");
    require(welcome != nullptr, "join should send welcome");
    requireEq(welcome->value("id", ""), std::string("p-1"), "first player id should be stable");
    requireEq(welcome->value("protocolVersion", 0), arena::protocolVersion, "welcome should expose protocol version");
    require(client.sawType("snapshot"), "join should send snapshot");
    const auto *initialSnapshot = client.firstType("snapshot");
    require(initialSnapshot != nullptr && initialSnapshot->value("full", false), "initial snapshot should be full");
    require(initialSnapshot != nullptr && initialSnapshot->value("snapshotId", 0) > 0, "snapshot should have id");
    require(initialSnapshot != nullptr && initialSnapshot->value("tick", -1) >= 0, "snapshot should have tick");

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
    requireEq(stats.value("protocolVersion", 0), arena::protocolVersion, "stats should expose protocol version");

    server.onClose(client.connection.get());
    const auto afterClose = server.healthJson();
    requireEq(afterClose.value("humans", 0), 0, "close should remove human");
    requireEq(afterClose.value("bots", 0), 0, "bots should leave when no humans remain");
  }

  void gameServerExposesStatsAndMetrics()
  {
    arena::GameServer server;
    CapturedClient client;
    require(server.onOpen(client.connection), "client should open");
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

  void gameServerLimitsConnectionAndProtocolAbuse()
  {
    arena::GameServer server;
    std::vector<std::shared_ptr<arena::ClientConnection>> clients;
    for (int i = 0; i < 16; ++i)
    {
      auto client = std::make_shared<arena::ClientConnection>();
      client->remoteAddress = "198.51.100.7";
      clients.push_back(client);
      require(server.onOpen(client), "connection should fit per-IP cap");
    }

    auto rejected = std::make_shared<arena::ClientConnection>();
    rejected->remoteAddress = "198.51.100.7";
    require(!server.onOpen(rejected), "connection above per-IP cap should be rejected");

    server.onClose(clients.back().get());
    require(server.onOpen(rejected), "connection should be accepted after one closes");

    CapturedClient abusive;
    require(server.onOpen(abusive.connection), "abusive test client should open");
    for (int i = 0; i < 5; ++i)
    {
      server.onMessage(abusive.connection.get(), "{");
    }
    require(abusive.closed, "too many invalid messages should close the session");

    CapturedClient burst;
    require(server.onOpen(burst.connection), "burst test client should open");
    for (int i = 0; i < 45; ++i)
    {
      server.onMessage(burst.connection.get(), R"({"type":"ping","t":1})");
    }
    const auto stats = server.statsJson();
    require(stats.at("websocket").value("rejectedConnections", 0) >= 1, "stats should count rejected connections");
    require(stats.at("websocket").value("protocolViolations", 0) >= 5, "stats should count protocol violations");
    require(stats.at("websocket").value("rateLimitRejects", 0) >= 1, "stats should count websocket message rate limits");

    const std::string metrics = server.metricsText();
    require(metrics.find("vix_arena_ws_rejected_connections_total") != std::string::npos, "metrics should expose connection rejects");
    require(metrics.find("vix_arena_ws_protocol_violations_total") != std::string::npos, "metrics should expose protocol violations");
  }

  void gameServerIsolatesRooms()
  {
    arena::GameServer server;
    CapturedClient alpha;
    CapturedClient bravo;
    require(server.onOpen(alpha.connection), "alpha client should open");
    require(server.onOpen(bravo.connection), "bravo client should open");

    server.onMessage(alpha.connection.get(), R"({"type":"join","name":"Alice","room":"alpha-room","protocolVersion":2})");
    server.onMessage(bravo.connection.get(), R"({"type":"join","name":"Bob","room":"bravo-room","protocolVersion":2})");

    const auto *alphaWelcome = alpha.firstType("welcome");
    const auto *bravoWelcome = bravo.firstType("welcome");
    require(alphaWelcome != nullptr && alphaWelcome->value("room", "") == "alpha-room", "alpha welcome should include room");
    require(bravoWelcome != nullptr && bravoWelcome->value("room", "") == "bravo-room", "bravo welcome should include room");

    const auto *bravoSnapshot = bravo.firstType("snapshot");
    require(bravoSnapshot != nullptr, "bravo should receive snapshot");
    for (const auto &player : bravoSnapshot->at("players"))
    {
      require(player.value("name", "") != "Alice", "bravo room should not see alpha player");
    }

    const std::size_t beforeBravoMessages = bravo.messages.size();
    server.onMessage(alpha.connection.get(), R"({"type":"chat","message":"alpha only"})");
    require(bravo.messages.size() == beforeBravoMessages, "chat should stay inside source room");

    const auto rooms = server.roomsJson();
    require(rooms.value("hiddenRooms", 0) >= 2, "rooms endpoint should count private rooms");
    for (const auto &room : rooms.at("rooms"))
    {
      const std::string code = room.contains("code") && room.at("code").is_string() ? room.at("code").get<std::string>() : "";
      require(code != "alpha-room", "rooms endpoint should not leak alpha room code");
      require(code != "bravo-room", "rooms endpoint should not leak bravo room code");
    }
  }

  void gameServerNegotiatesSnapshotDeltas()
  {
    arena::GameServer server;
    CapturedClient client;
    require(server.onOpen(client.connection), "client should open");
    server.onMessage(client.connection.get(), R"({"type":"join","name":"Delta","protocolVersion":2,"supports":["snapshot_delta"]})");

    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    server.stop();

    require(client.sawType("snapshot"), "delta client should receive initial full snapshot");
    require(client.sawType("snapshot_delta"), "delta client should receive negotiated snapshot deltas");

    const auto stats = server.statsJson();
    require(stats.at("websocket").value("snapshotDeltasSent", 0) > 0, "stats should count snapshot deltas");
    const std::string metrics = server.metricsText();
    require(metrics.find("vix_arena_ws_snapshot_deltas_sent_total") != std::string::npos, "metrics should expose snapshot deltas");
  }

  void gameServerLoadsPersistentLeaderboard()
  {
    const auto dir = std::filesystem::temp_directory_path() / ("vix-arena-test-" + std::to_string(arena::unixTimeMs()));
    std::filesystem::create_directories(dir);
    {
      std::ofstream out(dir / "vix-arena-state.json");
      out << R"({
        "schemaVersion": 1,
        "leaderboard": [
          {"name":"Micu","rounds":3,"wins":2,"totalScore":180,"bestScore":90,"lastPlayedAt":"2026-05-17T10:00:00Z"},
          {"name":"Ana","rounds":2,"wins":1,"totalScore":140,"bestScore":75,"lastPlayedAt":"2026-05-17T09:00:00Z"}
        ],
        "matches": [
          {"round":7,"room":"duel-room","endedAt":"2026-05-17T10:00:00Z","winner":{"id":"p-1","name":"Micu","score":90},"participants":[
            {"id":"p-1","name":"Micu","score":90,"bot":false,"winner":true},
            {"id":"p-2","name":"Ana","score":70,"bot":false,"winner":false}
          ]},
          {"round":8,"room":"public","endedAt":"2026-05-17T11:00:00Z","winner":{"id":"p-3","name":"Public","score":50},"participants":[
            {"id":"p-3","name":"Public","score":50,"bot":false,"winner":true}
          ]}
        ]
      })";
    }

    arena::GameServer server(dir);
    const auto leaderboard = server.leaderboardJson();
    requireEq(leaderboard.at("entries").size(), std::size_t{2}, "leaderboard should load persisted entries");
    requireEq(leaderboard.at("entries").at(0).value("name", ""), std::string("Micu"), "leaderboard should sort by wins");
    requireEq(leaderboard.at("entries").at(0).value("wins", 0), 2, "leaderboard wins should load");

    const auto matches = server.matchesJson();
    requireEq(matches.at("matches").size(), std::size_t{2}, "match history should load persisted entries");
    requireEq(matches.at("matches").at(0).at("winner").value("name", ""), std::string("Micu"), "match winner should load");
    const auto roomMatches = server.matchesJson("duel-room");
    requireEq(roomMatches.at("room").get<std::string>(), std::string("duel-room"), "room match filter should echo room");
    requireEq(roomMatches.at("matches").size(), std::size_t{1}, "room match filter should only include requested room");
    const auto roomLeaderboard = server.leaderboardJson("duel-room");
    requireEq(roomLeaderboard.at("room").get<std::string>(), std::string("duel-room"), "room leaderboard should echo room");
    requireEq(roomLeaderboard.at("entries").size(), std::size_t{2}, "room leaderboard should aggregate match participants");
    requireEq(roomLeaderboard.at("entries").at(0).value("name", ""), std::string("Micu"), "room leaderboard should sort winner first");

    const auto stats = server.statsJson();
    require(stats.at("persistence").value("enabled", false), "persistence should be enabled when data dir is configured");
    requireEq(stats.at("persistence").value("leaderboardEntries", 0), 2, "stats should expose persistent leaderboard size");

    std::filesystem::remove_all(dir);
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
    run("game server limits connection and protocol abuse", gameServerLimitsConnectionAndProtocolAbuse);
    run("game server isolates rooms", gameServerIsolatesRooms);
    run("game server negotiates snapshot deltas", gameServerNegotiatesSnapshotDeltas);
    run("game server loads persistent leaderboard", gameServerLoadsPersistentLeaderboard);
  }
  catch (const std::exception &)
  {
    return 1;
  }

  std::cout << testsRun << " tests passed\n";
  return 0;
}
