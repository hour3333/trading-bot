#pragma once

#include "trading_bot/logger.hpp"
#include "trading_bot/backtest_engine.hpp"
#include "trading_bot/market_data.hpp"
#include "trading_bot/telegram_api.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trading_bot {

class FinancialInsightService;
class Database;
class MonitoringScheduler;
class EconomicCalendarService;

struct SignalCommandSettings {
    std::string market{"XAU/USD"};
    std::string timeframe{"M5"};
    double minimumRiskReward{1.5};
    std::string timezone{"Asia/Phnom_Penh"};
};

class TelegramCommandHandler {
public:
    explicit TelegramCommandHandler(MarketDataService& marketDataService,
                                    FinancialInsightService* insightService = nullptr,
                                    Database* database = nullptr,
                                    BacktestSettings backtestSettings = {},
                                    MonitoringScheduler* scheduler = nullptr,
                                    EconomicCalendarService* calendarService = nullptr,
                                    SignalCommandSettings signalSettings = {});
    [[nodiscard]] std::string handle(
        std::string_view messageText,
        std::optional<std::int64_t> userId = std::nullopt) const;

private:
    MarketDataService* marketDataService_;
    FinancialInsightService* insightService_;
    Database* database_;
    BacktestSettings backtestSettings_;
    MonitoringScheduler* scheduler_;
    EconomicCalendarService* calendarService_;
    SignalCommandSettings signalSettings_;
};

class TelegramBot {
public:
    using OffsetStore = std::function<void(std::int64_t)>;

    TelegramBot(TelegramApiClient& apiClient, Logger& logger,
                MarketDataService& marketDataService,
                std::vector<std::int64_t> allowedUserIds,
                long longPollTimeoutSeconds = 25,
                std::int64_t initialOffset = 0,
                OffsetStore offsetStore = {},
                FinancialInsightService* insightService = nullptr,
                Database* database = nullptr,
                BacktestSettings backtestSettings = {},
                MonitoringScheduler* scheduler = nullptr,
                EconomicCalendarService* calendarService = nullptr,
                SignalCommandSettings signalSettings = {});

    // Runs until terminated. Transient transport/API failures are retried with
    // capped exponential backoff; Telegram flood-control delays are respected.
    void run(const std::function<bool()>& stopRequested = {});

    // One poll cycle is exposed separately for deterministic mocked tests.
    void pollOnce();

    [[nodiscard]] std::int64_t nextOffset() const noexcept { return nextOffset_; }

private:
    void handleUpdate(const TelegramUpdate& update);
    [[nodiscard]] bool isAuthorized(std::int64_t userId) const;

    TelegramApiClient* apiClient_;
    Logger* logger_;
    std::vector<std::int64_t> allowedUserIds_;
    long longPollTimeoutSeconds_;
    std::int64_t nextOffset_{0};
    OffsetStore offsetStore_;
    TelegramCommandHandler commandHandler_;
};

} // namespace trading_bot
