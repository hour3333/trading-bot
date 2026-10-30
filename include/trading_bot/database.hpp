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
    void saveSignalScenario(const SignalScenario& scenario);
    void saveBacktestResult(const BacktestResult& result);
    [[nodiscard]] std::vector<SimulatedTrade> loadRecentBacktestTrades(
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
        std::chrono::system_clock::time_point createdAt);
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
