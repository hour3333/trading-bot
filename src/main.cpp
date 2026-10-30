#include "trading_bot/config.hpp"
#include "trading_bot/backtest_engine.hpp"
#include "trading_bot/database.hpp"
#include "trading_bot/economic_calendar.hpp"
#include "trading_bot/error.hpp"
#include "trading_bot/financial_insight.hpp"
#include "trading_bot/financial_news.hpp"
#include "trading_bot/gemini_client.hpp"
#include "trading_bot/http_client.hpp"
#include "trading_bot/integration_verifier.hpp"
#include "trading_bot/logger.hpp"
#include "trading_bot/monitoring_scheduler.hpp"
#include "trading_bot/provider_market_data_service.hpp"
#include "trading_bot/telegram_api.hpp"
#include "trading_bot/telegram_bot.hpp"

#include <iostream>
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <thread>

namespace {
volatile std::sig_atomic_t shutdownRequested = 0;

void requestShutdown(int) {
    shutdownRequested = 1;
}

void configureTimezone(const std::string& timezone) {
    const std::filesystem::path zoneFile = std::filesystem::path("/usr/share/zoneinfo") / timezone;
    const std::filesystem::path macZoneFile = std::filesystem::path("/var/db/timezone/zoneinfo") / timezone;
    if (!std::filesystem::exists(zoneFile) && !std::filesystem::exists(macZoneFile)) {
        throw trading_bot::AppError(trading_bot::ErrorCode::Configuration,
                                    "BOT_TIMEZONE does not name an installed IANA timezone");
    }
    if (::setenv("TZ", timezone.c_str(), 1) != 0) {
        throw trading_bot::AppError(trading_bot::ErrorCode::Configuration,
                                    "Unable to apply BOT_TIMEZONE");
    }
    ::tzset();
}
} // namespace

int main(int argc, char** argv) {
    try {
        bool verifyOnly = false;
        std::filesystem::path dotenvPath = ".env";
        if (argc > 1) {
            if (std::string_view(argv[1]) != "--verify-integrations") {
                throw trading_bot::AppError(
                    trading_bot::ErrorCode::Configuration,
                    "Supported options: none, --verify-integrations [--env-file PATH]");
            }
            verifyOnly = true;
            if (argc > 2) {
                if (argc != 4 || std::string_view(argv[2]) != "--env-file" ||
                    std::string_view(argv[3]).empty()) {
                    throw trading_bot::AppError(
                        trading_bot::ErrorCode::Configuration,
                        "Usage: trading_bot --verify-integrations [--env-file PATH]");
                }
                dotenvPath = argv[3];
            }
        }
        const auto config = trading_bot::Config::load(dotenvPath);
        configureTimezone(config.timezone);
        std::signal(SIGINT, requestShutdown);
        std::signal(SIGTERM, requestShutdown);
        if (verifyOnly) {
            trading_bot::Database temporaryDatabase(":memory:");
            const trading_bot::HttpClient verificationHttp(config.httpTimeoutSeconds);
            const auto checks = trading_bot::verifyIntegrations(config, temporaryDatabase,
                                                                 verificationHttp);
            std::cout << trading_bot::formatIntegrationReport(checks);
            return 0;
        }
        trading_bot::Logger logger(config.logLevel, std::clog);
        trading_bot::Database database(config.databasePath);
        if (!config.telegramBotToken) {
            logger.warning("telegram.not_configured", {
                {"required_environment_variable", "TELEGRAM_BOT_TOKEN"},
            });
            return 0;
        }
        if (config.telegramAllowedUserIds.empty()) {
            throw trading_bot::AppError(
                trading_bot::ErrorCode::Configuration,
                "TELEGRAM_ALLOWED_USER_IDS must contain at least one authorized Telegram user ID");
        }

        const long httpTimeout = std::max(config.httpTimeoutSeconds,
                                          config.telegramLongPollTimeoutSeconds + 5);
        const trading_bot::HttpClient telegramPollHttp(httpTimeout);
        const trading_bot::HttpClient serviceHttp(config.httpTimeoutSeconds);
        trading_bot::TelegramApiClient telegramApi(*config.telegramBotToken, telegramPollHttp);
        trading_bot::TelegramApiClient alertTelegramApi(*config.telegramBotToken, serviceHttp);
        trading_bot::RssFinancialNewsService financialNews(database, serviceHttp);
        trading_bot::BlsEconomicCalendarService economicCalendar(database, serviceHttp);
        std::optional<trading_bot::GeminiClient> geminiClient;
        if (config.geminiApiKey) {
            geminiClient.emplace(*config.geminiApiKey, serviceHttp);
        }
        trading_bot::FinancialInsightService financialInsights(
            database, financialNews, geminiClient ? &*geminiClient : nullptr,
            static_cast<std::size_t>(config.newsGeminiSummariesPerHour));
        trading_bot::ProviderMarketDataService marketData(
            database, serviceHttp, config.coingeckoDemoApiKey, config.goldApiKey);
        const trading_bot::BacktestSettings backtestSettings{
            config.backtestStartingBalance,
            config.backtestFeeBasisPoints,
            config.backtestSlippageBasisPoints,
            config.backtestRiskFraction,
            config.signalMinimumRiskReward,
        };
        trading_bot::MonitoringSettings monitoringSettings;
        monitoringSettings.marketCheckInterval =
            std::chrono::seconds(config.marketCheckIntervalSeconds);
        monitoringSettings.newsCheckInterval =
            std::chrono::seconds(config.newsCheckIntervalSeconds);
        monitoringSettings.newsNotificationsPerHour =
            static_cast<std::size_t>(config.newsNotificationsPerHour);
        monitoringSettings.newsGeminiSummariesPerHour =
            static_cast<std::size_t>(config.newsGeminiSummariesPerHour);
        monitoringSettings.timezone = config.timezone;
        monitoringSettings.signalMarket = trading_bot::MarketSymbol::XauUsd;
        monitoringSettings.signalTimeframe = config.signalTimeframe;
        monitoringSettings.minimumRiskReward = config.signalMinimumRiskReward;
        monitoringSettings.signalAlertCooldown =
            std::chrono::seconds(config.signalAlertCooldownSeconds);
        monitoringSettings.alertMode = config.alertMode;
        monitoringSettings.tradingMode = config.tradingMode;
        monitoringSettings.automaticSignalAnalysis = config.automaticSignalAnalysis;
        monitoringSettings.allowDuplicateSignalAlerts = config.allowDuplicateSignalAlerts;
        const trading_bot::SignalCommandSettings signalCommandSettings{
            config.signalMarket, config.signalTimeframe,
            config.signalMinimumRiskReward, config.timezone};
        trading_bot::MonitoringScheduler scheduler(
            database, marketData, financialNews, alertTelegramApi, logger,
            config.telegramAllowedUserIds, monitoringSettings, &financialInsights);
        const auto nextOffset = database.metadataInt64("telegram_next_update_offset").value_or(0);
        trading_bot::TelegramBot bot(
            telegramApi, logger, marketData, config.telegramAllowedUserIds,
            config.telegramLongPollTimeoutSeconds, nextOffset,
            [&database](std::int64_t offset) {
                database.setMetadataInt64("telegram_next_update_offset", offset);
            },
            &financialInsights, &database, backtestSettings, &scheduler, &economicCalendar,
            signalCommandSettings);

        std::jthread schedulerThread([&scheduler](std::stop_token stopToken) {
            scheduler.run(stopToken);
        });

        logger.info("telegram.polling_started", {
            {"allowed_user_count", config.telegramAllowedUserIds.size()},
            {"http_timeout_seconds", httpTimeout},
            {"service_http_timeout_seconds", config.httpTimeoutSeconds},
            {"long_poll_timeout_seconds", config.telegramLongPollTimeoutSeconds},
            {"market_check_interval_seconds", config.marketCheckIntervalSeconds},
            {"news_check_interval_seconds", config.newsCheckIntervalSeconds},
            {"news_alerts_per_hour", config.newsNotificationsPerHour},
            {"news_gemini_summaries_per_hour", config.newsGeminiSummariesPerHour},
            {"timezone", config.timezone},
            {"signal_market", config.signalMarket},
            {"signal_timeframe", config.signalTimeframe},
            {"alert_mode", config.alertMode},
            {"trading_mode", config.tradingMode},
        });
        bot.run([] { return shutdownRequested != 0; });
        schedulerThread.request_stop();
        schedulerThread.join();
        logger.info("application.shutdown", {{"status", "graceful"}});
        return 0;
    } catch (const trading_bot::AppError& error) {
        std::cerr << nlohmann::json{{"level", "error"},
                                    {"error_code", trading_bot::errorCodeName(error.code())},
                                    {"message", error.what()}}
                         .dump()
                  << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << nlohmann::json{{"level", "error"}, {"error_code", "internal"},
                                    {"message", error.what()}}
                         .dump()
                  << '\n';
        return 1;
    }
}
