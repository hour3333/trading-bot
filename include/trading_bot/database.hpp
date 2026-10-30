#pragma once

#include "trading_bot/market_data.hpp"
#include "trading_bot/signal_engine.hpp"
#include "trading_bot/backtest_engine.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <mutex>
#include <sqlite3.h>
#include <string>
#include <string_view>
#include <vector>

namespace trading_bot {

struct ProviderCacheEntry {
    std::string payload;
    std::chrono::system_clock::time_point fetchedAt;
};

struct AiSummaryCacheEntry {
    std::string summary;
    std::chrono::system_clock::time_point generatedAt;
};

struct AlertDelivery {
    std::int64_t alertId{0};
    std::int64_t userId{0};
    std::string message;
    std::size_t attempts{0};
};

struct NewsNotificationPreference {
    bool enabled{true};
    bool highImpactOnly{false};
};

struct AnalysisNotificationPreference {
    bool enabled{false};
};

struct GoldAnalysisHistoryEntry {
    std::chrono::system_clock::time_point analyzedAt{};
    std::optional<std::chrono::system_clock::time_point> candleTimestamp;
    std::string status;
    std::string trend;
    std::string signalState;
    std::string source;
    std::string reason;
    std::string trigger;
    std::string changeKey;
    std::string message;
};

struct NewsStoryDecision {
    std::string storyKey;
    int revision{1};
    bool shouldNotify{false};
    bool substantialUpdate{false};
};

struct SignalHistoryEntry {
    MarketSymbol market{MarketSymbol::XauUsd};
    std::chrono::system_clock::time_point analysisAt{};
    std::string timeframe;
    std::string trend;
    std::string state;
    std::string dataStatus;
    std::optional<double> entryLow;
    std::optional<double> entryHigh;
    std::optional<double> stopLoss;
    std::optional<double> takeProfit1;
    std::optional<double> takeProfit2;
    std::optional<double> riskReward1;
    std::optional<double> riskReward2;
    std::string dataSource;
    std::optional<std::chrono::system_clock::time_point> candleTimestamp;
};

class Database {
public:
    explicit Database(const std::filesystem::path& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    void execute(std::string_view sql);
    [[nodiscard]] std::int64_t queryInt64(std::string_view sql) const;
    [[nodiscard]] std::optional<std::int64_t> metadataInt64(std::string_view key) const;
    void setMetadataInt64(std::string_view key, std::int64_t value);
    [[nodiscard]] std::optional<std::string> metadataText(std::string_view key) const;
    void setMetadataText(std::string_view key, std::string_view value);
    [[nodiscard]] std::optional<ProviderCacheEntry> loadProviderCache(std::string_view key) const;
    void saveProviderCache(std::string_view key, std::string_view payload,
                           std::chrono::system_clock::time_point fetchedAt);
    [[nodiscard]] std::optional<AiSummaryCacheEntry> loadAiSummary(std::string_view key) const;
    void saveAiSummary(std::string_view key, std::string_view summary,
                       std::chrono::system_clock::time_point generatedAt);
    [[nodiscard]] bool reserveGoldNewsGeminiCall(
        std::chrono::system_clock::time_point requestedAt, std::size_t maxPerHour);
    [[nodiscard]] NewsNotificationPreference newsNotificationPreference(
        std::int64_t userId) const;
    void setNewsNotificationPreference(std::int64_t userId,
                                       NewsNotificationPreference preference,
                                       std::chrono::system_clock::time_point updatedAt);
    [[nodiscard]] AnalysisNotificationPreference analysisNotificationPreference(
        std::int64_t userId) const;
    void setAnalysisNotificationPreference(std::int64_t userId,
                                           AnalysisNotificationPreference preference,
                                           std::chrono::system_clock::time_point updatedAt);
    [[nodiscard]] std::vector<std::int64_t> analysisNotificationRecipients(
        const std::vector<std::int64_t>& allowedUserIds) const;
    void saveGoldAnalysisHistory(const GoldAnalysisHistoryEntry& entry);
    [[nodiscard]] std::optional<GoldAnalysisHistoryEntry> latestGoldAnalysisHistory() const;
    [[nodiscard]] NewsStoryDecision recordNewsStory(
        std::string_view storyKey, std::string_view articleId, std::string_view url,
        std::string_view headline, std::string_view source,
        std::chrono::system_clock::time_point publishedAt,
        std::chrono::system_clock::time_point detectedAt, bool baselineOnly);
    [[nodiscard]] std::vector<std::int64_t> eligibleNewsRecipients(
        std::string_view impact, const std::vector<std::int64_t>& userIds,
        std::chrono::system_clock::time_point createdAt, std::size_t maxPerHour) const;
    [[nodiscard]] bool enqueueNewsAlert(
        std::string_view dedupeKey, std::string_view impact, std::string_view storyKey,
        int revision, std::string_view message, std::string_view sourceUrl,
        const std::vector<std::int64_t>& userIds,
        std::chrono::system_clock::time_point createdAt, std::size_t maxPerHour);
    void saveSignalScenario(const SignalScenario& scenario);
    void saveBacktestResult(const BacktestResult& result);
    [[nodiscard]] std::vector<SimulatedTrade> loadRecentBacktestTrades(
        std::size_t limit = 10) const;
    [[nodiscard]] std::vector<SignalHistoryEntry> loadRecentSignalScenarios(
        std::size_t limit = 10) const;
    void replaceMarketCandles(std::string_view cacheKey, const std::vector<OhlcCandle>& candles);
    [[nodiscard]] std::vector<OhlcCandle> loadMarketCandles(std::string_view cacheKey) const;

    // Alert payloads contain public headlines or public market scenarios only.
    // Delivery recipients must be supplied from the configured Telegram allowlist.
    [[nodiscard]] bool enqueueAlert(std::string_view dedupeKey, std::string_view category,
                                   std::string_view message, std::string_view sourceUrl,
                                   const std::vector<std::int64_t>& userIds,
                                   std::chrono::system_clock::time_point createdAt);
    [[nodiscard]] bool recordSignalTransition(
        std::string_view market, std::string_view state, std::string_view dedupeKey,
        std::string_view message, const std::vector<std::int64_t>& userIds,
        std::chrono::system_clock::time_point createdAt,
        std::chrono::seconds cooldown = std::chrono::seconds::zero(),
        bool allowSameStateAlert = false);
    [[nodiscard]] std::vector<AlertDelivery> loadDueAlertDeliveries(
        std::chrono::system_clock::time_point asOf, std::size_t limit = 20) const;
    void markAlertDelivered(std::int64_t alertId, std::int64_t userId,
                            std::chrono::system_clock::time_point deliveredAt);
    void deferAlertDelivery(std::int64_t alertId, std::int64_t userId,
                            std::chrono::system_clock::time_point nextAttempt,
                            std::string_view errorCategory, bool permanent);
    void recordSchedulerError(std::string_view component, std::string_view errorCategory,
                              std::chrono::system_clock::time_point occurredAt);
    [[nodiscard]] std::optional<std::string> lastSchedulerError() const;

private:
    sqlite3* handle_{nullptr};
    // Transactions span multiple SQLite calls; serialize complete public
    // operations, not just individual FULLMUTEX SQLite calls.
    mutable std::recursive_mutex mutex_;
};

} // namespace trading_bot
