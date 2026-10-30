#pragma once

#include "trading_bot/logger.hpp"

#include <filesystem>
#include <optional>
#include <cstdint>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading_bot {

struct Config {
    std::optional<std::string> telegramBotToken;
    std::vector<std::int64_t> telegramAllowedUserIds;
    std::optional<std::string> geminiApiKey;
    std::optional<std::string> coingeckoDemoApiKey;
    std::optional<std::string> goldApiKey;
    std::filesystem::path databasePath{"data/trading_bot.sqlite3"};
    long httpTimeoutSeconds{20};
    long telegramLongPollTimeoutSeconds{25};
    long marketCheckIntervalSeconds{300};
    long newsCheckIntervalSeconds{300};
    long newsNotificationsPerHour{6};
    long newsGeminiSummariesPerHour{6};
    std::string timezone{"Asia/Phnom_Penh"};
    std::string signalMarket{"XAU/USD"};
    std::string signalTimeframe{"M5"};
    std::string alertMode{"CONFIRMED_ONLY"};
    std::string tradingMode{"PAPER_ONLY"};
    std::string aiProvider{"GEMINI"};
    bool automaticSignalAnalysis{true};
    bool allowDuplicateSignalAlerts{false};
    long signalAlertCooldownSeconds{300};
    double signalMinimumRiskReward{1.5};
    LogLevel logLevel{LogLevel::Info};
    double backtestStartingBalance{10'000.0};
    double backtestFeeBasisPoints{10.0};
    double backtestSlippageBasisPoints{5.0};
    double backtestRiskFraction{0.01};

    // Reads an optional dotenv file, then overlays actual process environment
    // variables so shell/IDE settings always take precedence.
    [[nodiscard]] static Config load(const std::filesystem::path& dotenvPath = ".env");
    [[nodiscard]] static Config fromValues(
        const std::unordered_map<std::string, std::string>& values);
};

} // namespace trading_bot
