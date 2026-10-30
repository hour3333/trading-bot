#pragma once

#include "trading_bot/database.hpp"
#include "trading_bot/financial_news.hpp"
#include "trading_bot/financial_insight.hpp"
#include "trading_bot/logger.hpp"
#include "trading_bot/market_data.hpp"
#include "trading_bot/telegram_api.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading_bot {

struct MonitoringSettings {
    std::chrono::seconds marketCheckInterval{std::chrono::minutes(5)};
    std::chrono::seconds newsCheckInterval{std::chrono::minutes(5)};
    std::string timezone{"Asia/Phnom_Penh"};
    MarketSymbol signalMarket{MarketSymbol::XauUsd};
    std::string signalTimeframe{"M5"};
    double minimumRiskReward{1.5};
    std::chrono::seconds signalAlertCooldown{std::chrono::minutes(5)};
    std::string alertMode{"CONFIRMED_ONLY"};
    std::string tradingMode{"PAPER_ONLY"};
    bool automaticSignalAnalysis{true};
    bool allowDuplicateSignalAlerts{false};
    std::size_t newsNotificationsPerHour{6};
    std::size_t newsGeminiSummariesPerHour{6};
};

// Local scheduler for public market/news monitoring. All outbound Telegram
// messages are delivered only to the configured private-user allowlist.
class MonitoringScheduler {
public:
    MonitoringScheduler(Database& database, MarketDataService& marketData,
                        FinancialNewsService& news, TelegramApiClient& telegram,
                        Logger& logger, std::vector<std::int64_t> allowedUserIds,
                        MonitoringSettings settings = {},
                        FinancialInsightService* insights = nullptr);

    void run(std::stop_token stopToken);
    // One due-check and delivery pass; exposed to permit deterministic tests.
    void tick(std::chrono::system_clock::time_point now =
                  std::chrono::system_clock::now());
    // Forces both data checks and a delivery pass, regardless of prior schedule.
    void checkNow(std::chrono::system_clock::time_point now =
                      std::chrono::system_clock::now());
    [[nodiscard]] std::string statusMessage() const;
    [[nodiscard]] std::string newsStatusMessage(std::int64_t userId) const;
    [[nodiscard]] std::string analysisStatusMessage(std::int64_t userId) const;

private:
    void checkMarkets(std::chrono::system_clock::time_point now,
                      bool importantNewsDetected = false,
                      bool runGoldAnalysis = false);
    [[nodiscard]] bool checkNews(std::chrono::system_clock::time_point now);
    void refreshGoldM5Analysis(std::chrono::system_clock::time_point now,
                               bool importantNewsDetected,
                               const MarketDataSnapshot* reusableSnapshot = nullptr);
    void deliverPending(std::chrono::system_clock::time_point now);
    void recordFailure(std::string_view component, std::string_view category,
                       std::chrono::system_clock::time_point now) noexcept;

    Database* database_;
    MarketDataService* marketData_;
    FinancialNewsService* news_;
    FinancialInsightService* insights_;
    TelegramApiClient* telegram_;
    Logger* logger_;
    std::vector<std::int64_t> allowedUserIds_;
    MonitoringSettings settings_;
    std::chrono::system_clock::time_point processStartedAt_{
        std::chrono::system_clock::now()};
    mutable std::mutex statusMutex_;
    bool running_{false};
    std::optional<std::chrono::system_clock::time_point> lastMarketCheck_;
    std::optional<std::chrono::system_clock::time_point> lastNewsCheck_;
    std::optional<std::chrono::system_clock::time_point> lastGoldAnalysisCheck_;
    std::optional<NewsSnapshot> latestNewsSnapshot_;
    std::unordered_map<std::int64_t, std::chrono::system_clock::time_point> lastDeliveryByUser_;
};

} // namespace trading_bot
