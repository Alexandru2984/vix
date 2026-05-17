#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "Player.hpp"
#include "Persistence.hpp"
#include "World.hpp"

namespace arena
{
  class GameServer
  {
  public:
    explicit GameServer(std::filesystem::path dataDir = {}, std::string databaseUrl = {});
    ~GameServer();

    GameServer(const GameServer &) = delete;
    GameServer &operator=(const GameServer &) = delete;

    void start();
    void stop();

    void onOpen(const std::shared_ptr<ClientConnection> &session);
    void onClose(ClientConnection *session);
    void onMessage(ClientConnection *session, const std::string &payload);

    [[nodiscard]] nlohmann::json healthJson() const;
    [[nodiscard]] nlohmann::json stateJson() const;
    [[nodiscard]] nlohmann::json statsJson() const;
    [[nodiscard]] nlohmann::json leaderboardJson() const;
    [[nodiscard]] nlohmann::json matchesJson() const;
    [[nodiscard]] std::string metricsText() const;

    [[nodiscard]] int tickRateTarget() const noexcept { return tickRateTarget_; }

  private:
    struct Orb
    {
      std::string id;
      double x{0.0};
      double y{0.0};
      int value{5};
      std::string color{"#66ccff"};
    };

    struct Powerup
    {
      std::string id;
      std::string kind{"speed"};
      double x{0.0};
      double y{0.0};
      double durationSeconds{6.0};
      std::string color{"#c9a7ff"};
    };

    struct GameEvent
    {
      std::uint64_t id{0};
      std::string type;
      std::string text;
      std::string timestamp;
    };

    struct ClientProtocolState
    {
      int version{1};
      bool supportsSnapshotDelta{false};
      std::uint64_t lastSnapshotId{0};
      nlohmann::json lastSnapshot;
    };

    struct LeaderboardEntry
    {
      std::string name;
      std::uint64_t rounds{0};
      std::uint64_t wins{0};
      std::uint64_t totalScore{0};
      int bestScore{0};
      std::string lastPlayedAt;
    };

    using SessionPtr = std::shared_ptr<ClientConnection>;
    using PreparedPayload = std::pair<SessionPtr, std::string>;

    void handleJoin(ClientConnection *session, const nlohmann::json &message);
    void handleInput(ClientConnection *session, const nlohmann::json &message);
    void handleChat(ClientConnection *session, const nlohmann::json &message);
    void handlePing(ClientConnection *session, const nlohmann::json &message);
    void handleAbility(ClientConnection *session, const nlohmann::json &message);

    void tickLoop();
    void step(double dt);
    void ensureBotsLocked(std::chrono::steady_clock::time_point now);
    void removeBotsLocked();
    void updateBotsLocked(std::chrono::steady_clock::time_point now);
    [[nodiscard]] Player spawnBotLocked(std::chrono::steady_clock::time_point now);
    void chooseBotTargetLocked(Player &bot, std::chrono::steady_clock::time_point now);
    [[nodiscard]] std::size_t humanCountLocked() const;
    [[nodiscard]] std::size_t botCountLocked() const;
    void ensureOrbsLocked();
    void ensurePowerupsLocked();
    void updateRoundLocked();
    void finishRoundLocked(std::chrono::steady_clock::time_point now);
    void startNextRoundLocked(std::chrono::steady_clock::time_point now);
    void loadPersistentStateLocked();
    void savePersistentStateLocked() const;
    void recordRoundLocked();
    [[nodiscard]] MatchRecord matchRecordLocked(const std::string &endedAt) const;
    [[nodiscard]] nlohmann::json leaderboardJsonLocked(std::size_t limit = 10) const;
    [[nodiscard]] nlohmann::json matchesJsonLocked(std::size_t limit = 20) const;
    void handleOrbPickupsLocked();
    void handlePowerupPickupsLocked();
    void handleControlZoneLocked(double dt);
    void applyDashLocked(Player &player, std::chrono::steady_clock::time_point now);
    void addEventLocked(std::string type, std::string text);
    void cleanupStaleLocked(std::vector<nlohmann::json> &leftEvents);
    [[nodiscard]] nlohmann::json snapshotLocked(std::uint64_t tick, std::uint64_t snapshotId) const;
    [[nodiscard]] nlohmann::json snapshotDeltaLocked(const nlohmann::json &current, const nlohmann::json &previous, std::uint64_t baseSnapshotId) const;
    [[nodiscard]] nlohmann::json orbsJsonLocked() const;
    [[nodiscard]] nlohmann::json powerupsJsonLocked() const;
    [[nodiscard]] nlohmann::json controlZoneJson() const;
    [[nodiscard]] nlohmann::json roundJsonLocked() const;
    [[nodiscard]] nlohmann::json eventsJsonLocked() const;
    [[nodiscard]] std::vector<SessionPtr> liveSessionsLocked() const;
    [[nodiscard]] std::vector<PreparedPayload> snapshotPayloadsLocked(const std::vector<SessionPtr> &sessions, const nlohmann::json &snapshot);

    void send(ClientConnection *session, const nlohmann::json &message);
    void broadcast(const nlohmann::json &message);
    void broadcastTo(const std::vector<SessionPtr> &sessions, const nlohmann::json &message);
    void sendPrepared(const std::vector<PreparedPayload> &payloads, bool snapshotLike);
    void recordTickDurationLocked(std::uint64_t durationUs);

    [[nodiscard]] std::string randomColor();
    [[nodiscard]] Orb spawnOrbLocked();
    [[nodiscard]] Powerup spawnPowerupLocked();

    mutable std::mutex mutex_;
    World world_;
    std::unordered_map<std::string, Player> players_;
    std::unordered_map<ClientConnection *, std::string> sessionToPlayer_;
    std::unordered_map<ClientConnection *, ClientProtocolState> sessionProtocol_;
    std::deque<nlohmann::json> chatHistory_;
    std::deque<GameEvent> eventHistory_;
    std::unordered_map<std::string, LeaderboardEntry> leaderboard_;
    std::deque<nlohmann::json> matchHistory_;
    std::vector<Orb> orbs_;
    std::vector<Powerup> powerups_;
    std::mt19937 rng_;
    std::filesystem::path dataDir_;
    std::filesystem::path stateFile_;
    std::unique_ptr<PersistenceStore> persistence_;

    std::atomic<bool> running_{false};
    std::thread tickThread_;
    std::chrono::steady_clock::time_point startedAt_;
    std::uint64_t nextPlayerNumber_{1};
    std::uint64_t totalConnections_{0};
    std::uint64_t totalChatMessages_{0};
    std::uint64_t totalOrbPickups_{0};
    std::uint64_t totalControlZonePoints_{0};
    std::uint64_t totalPowerupsSinceStart_{0};
    std::uint64_t totalQuestsCompleted_{0};
    std::uint64_t nextOrbNumber_{1};
    std::uint64_t nextPowerupNumber_{1};
    std::uint64_t nextBotNumber_{1};
    std::uint64_t nextEventNumber_{1};
    std::uint64_t roundNumber_{1};
    std::uint64_t totalRoundsCompleted_{0};
    std::uint64_t currentTick_{0};
    std::uint64_t nextSnapshotId_{1};
    std::deque<std::uint64_t> recentTickDurationsUs_;
    std::uint64_t totalTicks_{0};
    std::uint64_t maxTickDurationUs_{0};
    std::chrono::steady_clock::time_point roundStartedAt_;
    std::chrono::steady_clock::time_point intermissionUntil_;
    bool intermission_{false};
    std::string lastWinnerId_;
    std::string lastWinnerName_{"No winner yet"};
    int lastWinnerScore_{0};

    static constexpr int tickRateTarget_{20};
    static constexpr std::size_t maxPlayers_{64};
    static constexpr std::size_t targetPlayersWithBots_{4};
    static constexpr std::size_t maxBots_{3};
    static constexpr int orbQuestGoal_{3};
    static constexpr int orbQuestReward_{20};
    static constexpr double playerSpeed_{235.0};
    static constexpr std::size_t targetOrbCount_{18};
    static constexpr std::size_t targetPowerupCount_{5};
    static constexpr double orbRadius_{12.0};
    static constexpr double powerupRadius_{14.0};
    static constexpr double speedBoostMultiplier_{1.55};
    static constexpr double dashDistance_{170.0};
    static constexpr int dashCooldownMs_{2600};
    static constexpr int shieldDurationMs_{3200};
    static constexpr int shieldCooldownMs_{10000};
    static constexpr int magnetDurationMs_{5200};
    static constexpr int magnetCooldownMs_{12000};
    static constexpr double magnetRadius_{210.0};
    static constexpr double controlZoneX_{1000.0};
    static constexpr double controlZoneY_{600.0};
    static constexpr double controlZoneRadius_{150.0};
    static constexpr double controlPointsPerSecond_{2.0};
    static constexpr int roundDurationSeconds_{180};
    static constexpr int intermissionSeconds_{10};
    static constexpr std::size_t tickDurationSampleLimit_{512};
    static constexpr std::size_t matchHistoryLimit_{50};

    std::atomic<std::uint64_t> totalMessagesReceived_{0};
    std::atomic<std::uint64_t> totalMessageBytesReceived_{0};
    std::atomic<std::uint64_t> totalMessagesSent_{0};
    std::atomic<std::uint64_t> totalMessageBytesSent_{0};
    std::atomic<std::uint64_t> totalSnapshotsSent_{0};
    std::atomic<std::uint64_t> totalSnapshotBytesSent_{0};
    std::atomic<std::uint64_t> totalSnapshotDeltasSent_{0};
    std::atomic<std::uint64_t> totalSnapshotDeltaBytesSent_{0};
    std::atomic<std::uint64_t> totalRejectedMessages_{0};
    std::atomic<std::uint64_t> totalRateLimitRejects_{0};
    std::atomic<std::uint64_t> totalSendFailures_{0};
  };
}
