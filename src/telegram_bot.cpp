#include "trading_bot/telegram_bot.hpp"

#include "trading_bot/error.hpp"
#include "trading_bot/database.hpp"
#include "trading_bot/backtest_engine.hpp"
#include "trading_bot/financial_insight.hpp"
#include "trading_bot/financial_news.hpp"
#include "trading_bot/market_data_format.hpp"
#include "trading_bot/signal_engine.hpp"
#include "trading_bot/technical_analysis.hpp"
#include "trading_bot/telegram_messages.hpp"
#include "trading_bot/monitoring_scheduler.hpp"
#include "trading_bot/economic_calendar.hpp"
#include "trading_bot/gold_market_analysis.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>

namespace trading_bot {
namespace {

std::string_view commandToken(std::string_view messageText) {
    const auto first = messageText.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    messageText.remove_prefix(first);
    const auto end = messageText.find_first_of(" \t\r\n");
    return messageText.substr(0, end);
}

std::string normalizedCommand(std::string_view token) {
    if (token.empty() || token.front() != '/') {
        return {};
    }
    token.remove_prefix(1);
    const auto mention = token.find('@');
    if (mention != std::string_view::npos) {
        token = token.substr(0, mention);
    }
    std::string command(token);
    std::transform(command.begin(), command.end(), command.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return command;
}

std::chrono::seconds nextBackoff(std::chrono::seconds current) {
    constexpr auto maximumSeconds = std::chrono::seconds::rep{30};
    return std::chrono::seconds(std::min(current.count() * 2, maximumSeconds));
}

void sleepInterruptibly(std::chrono::seconds duration,
                       const std::function<bool()>& stopRequested) {
    auto remaining = duration;
    while (remaining > std::chrono::seconds::zero() &&
           (!stopRequested || !stopRequested())) {
        const auto step = std::min(remaining, std::chrono::seconds(1));
        std::this_thread::sleep_for(step);
        remaining -= step;
    }
}

NewsSnapshot goldRelevantNews(NewsSnapshot snapshot) {
    std::erase_if(snapshot.articles, [&](const NewsArticle& article) {
        return !isGoldRelevantNews(article);
    });
    return snapshot;
}

std::string formatLocalTimestamp(std::chrono::system_clock::time_point timestamp,
                                 std::string_view timezone) {
    const auto raw = std::chrono::system_clock::to_time_t(timestamp);
    std::tm local{};
    if (localtime_r(&raw, &local) == nullptr) return "DATA UNAVAILABLE";
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << ' ' << timezone;
    return output.str();
}

} // namespace

TelegramCommandHandler::TelegramCommandHandler(MarketDataService& marketDataService,
                                               FinancialInsightService* insightService,
                                               Database* database,
                                               BacktestSettings backtestSettings,
                                               MonitoringScheduler* scheduler,
                                               EconomicCalendarService* calendarService,
                                               SignalCommandSettings signalSettings)
    : marketDataService_(&marketDataService), insightService_(insightService),
      database_(database), backtestSettings_(backtestSettings), scheduler_(scheduler),
      calendarService_(calendarService), signalSettings_(std::move(signalSettings)) {}

std::string TelegramCommandHandler::handle(
    std::string_view messageText, std::optional<std::int64_t> userId) const {
    const auto command = normalizedCommand(commandToken(messageText));
    if (command == "start") {
        return formatWelcomeMessage();
    }
    if (command == "help") {
        return formatHelpMessage();
    }
    if (command == "gold" || command == "btc") {
        const auto symbol = command == "gold" ? MarketSymbol::XauUsd : MarketSymbol::BtcUsd;
        try {
            return formatMarketDataMessage(marketDataService_->getSnapshot(symbol));
        } catch (const std::exception&) {
            MarketDataSnapshot unavailable;
            unavailable.market = symbol;
            unavailable.historyDescription = "Historical OHLC";
            return formatMarketDataMessage(unavailable);
        }
    }
    if (command == "analysis" || command == "m5") {
        MarketDataSnapshot snapshot;
        snapshot.market = MarketSymbol::XauUsd;
        snapshot.historyDescription =
            "No genuine completed XAU/USD M5 candles are available from the configured provider.";
        try {
            snapshot = marketDataService_->getSnapshot(MarketSymbol::XauUsd, "M5");
        } catch (const std::exception&) {
            // The report preserves an explicit DATA UNAVAILABLE status.
        }
        NewsSnapshot news;
        if (insightService_ != nullptr) {
            try {
                news = insightService_->getLatestNews(20);
            } catch (const std::exception&) {
                news.unavailableSources.push_back("Configured financial news feeds");
            }
        }
        const auto now = std::chrono::system_clock::now();
        auto report = calculateGoldM5Analysis(snapshot, std::move(news), now,
                                               signalSettings_.minimumRiskReward);
        std::string assessment;
        if (report.status == "COMPLETE" && insightService_ != nullptr) {
            try {
                const auto explanation = insightService_->analyzeGoldM5(
                    report.technical, report.scenario, report.news);
                assessment = formatAiExplanation(explanation);
            } catch (const std::exception&) {
                assessment = "Gemini assessment unavailable; deterministic analysis is shown.";
            }
        }
        auto message = formatGoldM5AnalysisMessage(
            report, assessment, signalSettings_.timezone);
        if (database_ != nullptr) {
            try {
                GoldAnalysisHistoryEntry entry;
                entry.analyzedAt = now;
                if (report.technical.latestCandleEnd.time_since_epoch() >
                    std::chrono::system_clock::duration::zero()) {
                    entry.candleTimestamp = report.technical.latestCandleEnd;
                }
                entry.status = report.status;
                entry.trend = report.technical.status == AnalysisStatus::Complete
                    ? trendClassificationName(report.technical.trend) : "UNCONFIRMED";
                entry.signalState = report.scenario.state == SignalState::Wait
                    ? "WAIT" : signalStateName(report.scenario.state);
                entry.source = report.technical.source.empty()
                    ? report.reason : report.technical.source;
                entry.reason = report.reason;
                entry.trigger = "manual";
                entry.changeKey = goldAnalysisChangeKey(report);
                entry.message = message;
                database_->saveGoldAnalysisHistory(entry);
            } catch (const std::exception&) {
                message += "\nAnalysis history could not be saved to SQLite.";
            }
        }
        return message;
    }
    if (command == "analysis_h4") {
        std::vector<TechnicalAnalysisResult> analyses;
        analyses.reserve(2);
        for (const auto symbol : {MarketSymbol::BtcUsd, MarketSymbol::XauUsd}) {
            try {
                const auto snapshot = marketDataService_->getSnapshot(symbol, "H4");
                analyses.push_back(TechnicalAnalysis::analyze(symbol, snapshot.candles));
            } catch (const std::exception&) {
                analyses.push_back(TechnicalAnalysis::analyze(symbol, std::span<const OhlcCandle>{}));
            }
        }
        NewsSnapshot news;
        AiExplanation explanation;
        if (insightService_ != nullptr) {
            try {
                news = insightService_->getLatestNews();
                explanation = insightService_->analyzeMarkets(analyses, news);
            } catch (const std::exception&) {
                explanation.status = ExplanationStatus::ServiceUnavailable;
            }
        } else {
            explanation.status = ExplanationStatus::NotConfigured;
        }
        return formatMarketAnalysisMessage(analyses, news, explanation);
    }
    if (command == "news") {
        if (insightService_ == nullptr) {
            NewsSnapshot unavailable;
            return formatNewsMessage(unavailable) + "\n" +
                   formatAiExplanation(AiExplanation{
                       ExplanationStatus::NotConfigured, {}, {}, false});
        }
        try {
            const auto news = goldRelevantNews(insightService_->getLatestNews());
            const auto explanation = insightService_->explainNews(news);
            return formatNewsMessage(news, 3, 2500) + "\n" +
                   formatAiExplanation(explanation);
        } catch (const std::exception&) {
            NewsSnapshot unavailable;
            return formatNewsMessage(unavailable) + "\n" +
                   "AI commentary unavailable: news retrieval failed.";
        }
    }
    if (command == "news_on" || command == "news_off" ||
        command == "news_high" || command == "news_all") {
        if (!database_ || !userId || *userId <= 0) {
            return "News preferences are unavailable without an authorized Telegram user context.";
        }
        try {
            auto preference = database_->newsNotificationPreference(*userId);
            if (command == "news_on") preference.enabled = true;
            if (command == "news_off") preference.enabled = false;
            if (command == "news_high") {
                preference.enabled = true;
                preference.highImpactOnly = true;
            }
            if (command == "news_all") {
                preference.enabled = true;
                preference.highImpactOnly = false;
            }
            database_->setNewsNotificationPreference(
                *userId, preference, std::chrono::system_clock::now());
            const std::string scope = preference.highImpactOnly
                ? "HIGH impact only" : "HIGH and MEDIUM impact";
            return std::string("Gold news notifications are ") +
                (preference.enabled ? "ON" : "OFF") + ". Alert scope: " + scope + ".";
        } catch (const std::exception&) {
            return "Gold news preference could not be saved to local storage.";
        }
    }
    if (command == "news_status") {
        if (!userId || *userId <= 0) {
            return "News settings are available only in an authorized private chat.";
        }
        if (scheduler_ != nullptr) return scheduler_->newsStatusMessage(*userId);
        if (database_ == nullptr) return "Gold news settings are unavailable.";
        try {
            const auto preference = database_->newsNotificationPreference(*userId);
            return std::string("Gold news notifications: ") +
                (preference.enabled ? "ON" : "OFF") + "\nAlert scope: " +
                (preference.highImpactOnly ? "HIGH impact only" : "HIGH and MEDIUM impact") +
                "\nPolling interval: 300 seconds (default).";
        } catch (const std::exception&) {
            return "Gold news settings could not be read from local storage.";
        }
    }
    if (command == "news_test") {
        return "🔔 GOLD NEWS TEST — DIAGNOSTIC ONLY\n"
               "This is a test of the authorized Telegram notification path.\n"
               "No real article was detected, and this is not a market-impact assessment or trading signal.";
    }
    if (command == "analysis_on" || command == "analysis_off") {
        if (database_ == nullptr || !userId || *userId <= 0) {
            return "Analysis preferences are available only with an authorized Telegram user context.";
        }
        try {
            const bool enabled = command == "analysis_on";
            database_->setAnalysisNotificationPreference(
                *userId, AnalysisNotificationPreference{enabled},
                std::chrono::system_clock::now());
            return std::string("Gold M5 analysis notifications are ") +
                (enabled ? "ON" : "OFF") +
                ". Checks run every 300 seconds and on important verified gold news; "
                "only significant validated changes are sent. Alerts are educational and paper-only.";
        } catch (const std::exception&) {
            return "Gold M5 analysis preference could not be saved to local storage.";
        }
    }
    if (command == "analysis_status") {
        if (!userId || *userId <= 0) {
            return "Analysis settings are available only in an authorized private chat.";
        }
        if (scheduler_ != nullptr) return scheduler_->analysisStatusMessage(*userId);
        if (database_ == nullptr) return "Gold M5 analysis status is unavailable.";
        try {
            const auto preference = database_->analysisNotificationPreference(*userId);
            const auto latest = database_->latestGoldAnalysisHistory();
            if (!latest) {
                return std::string("Gold M5 analysis notifications: ") +
                    (preference.enabled ? "ON" : "OFF") +
                    "\nLast update: none yet\nData freshness: no scheduled/manual analysis has been recorded.";
            }
            const auto now = std::chrono::system_clock::now();
            std::ostringstream output;
            output << "Gold M5 analysis notifications: " << (preference.enabled ? "ON" : "OFF")
                   << "\nLast update: " << formatLocalTimestamp(
                       latest->analyzedAt, signalSettings_.timezone)
                   << "\nLast status: " << latest->status
                   << "\nTrend / scenario: " << latest->trend << " / " << latest->signalState
                   << "\nData freshness: ";
            if (!latest->candleTimestamp) {
                output << "DATA UNAVAILABLE (no completed M5 candle)";
            } else if (*latest->candleTimestamp > now) {
                output << "INVALID (future candle timestamp)";
            } else {
                const auto age = now - *latest->candleTimestamp;
                output << (latest->status == "COMPLETE" && age <= std::chrono::minutes(10)
                               ? "FRESH" : "STALE")
                       << " (completed candle " << formatLocalTimestamp(
                           *latest->candleTimestamp, signalSettings_.timezone) << ')';
            }
            if (isGoldWeeklyClosureExpected(now)) {
                output << "\nMarket session: weekend closure expected for many gold venues; exact hours vary by provider.";
            }
            return output.str();
        } catch (const std::exception&) {
            return "Gold M5 analysis status could not be read from local storage.";
        }
    }
    if (command == "calendar") {
        if (calendarService_ == nullptr) {
            return formatUnavailableMessage("/calendar");
        }
        try {
            return formatEconomicCalendarMessage(calendarService_->getUpcomingEvents());
        } catch (const std::exception&) {
            EconomicCalendarSnapshot unavailable;
            unavailable.fetchedAt = std::chrono::system_clock::now();
            unavailable.unavailable = true;
            return formatEconomicCalendarMessage(unavailable);
        }
    }
    std::string signalTimeframe;
    const bool configuredGoldSignal = command == "signal";
    if (configuredGoldSignal) signalTimeframe = signalSettings_.timeframe;
    else if (command == "signal_m5") signalTimeframe = "M5";
    else if (command == "signal_m15") signalTimeframe = "M15";
    else if (command == "signal_h1") signalTimeframe = "H1";
    else if (command == "signal_h4") signalTimeframe = "H4";
    if (!signalTimeframe.empty()) {
        std::ostringstream output;
        const std::vector<MarketSymbol> markets = configuredGoldSignal
            ? std::vector<MarketSymbol>{signalSettings_.market == "XAU/USD"
                                            ? MarketSymbol::XauUsd : MarketSymbol::BtcUsd}
            : std::vector<MarketSymbol>{MarketSymbol::BtcUsd, MarketSymbol::XauUsd};
        for (std::size_t index = 0; index < markets.size(); ++index) {
            const auto symbol = markets[index];
            MarketDataSnapshot snapshot;
            snapshot.market = symbol;
            snapshot.historyDescription = "Historical OHLC for " + signalTimeframe;
            try {
                snapshot = marketDataService_->getSnapshot(symbol, signalTimeframe);
            } catch (const std::exception&) {
                // Empty snapshot is evaluated as WAIT and saved without levels.
            }
            auto scenario = SignalEngine::evaluate(snapshot, signalTimeframe,
                std::chrono::system_clock::now(), signalSettings_.minimumRiskReward);
            bool stored = false;
            if (database_ != nullptr) {
                try {
                    database_->saveSignalScenario(scenario);
                    stored = true;
                } catch (const std::exception&) {
                    // Keep the safe WAIT/scenario response available even if local storage fails.
                }
            }
            if (index != 0) {
                output << "\n\n";
            }
            output << formatSignalScenario(scenario, signalSettings_.timezone);
            if (database_ != nullptr && !stored) {
                output << "\nScenario could not be saved to local storage.";
            }
            if (scenario.state == SignalState::Wait) {
                output << "\nAI explanation: not generated for WAIT; the deterministic reasons are shown above.";
                continue;
            }
            AiExplanation explanation;
            if (insightService_ != nullptr) {
                try {
                    if (configuredGoldSignal) {
                        auto news = goldRelevantNews(insightService_->getLatestNews());
                        if (!news.articles.empty()) {
                            output << "\n" << formatNewsMessage(news, 3, 1200);
                        }
                        explanation = insightService_->explainSignal(scenario, news);
                    } else {
                        explanation = insightService_->explainSignal(scenario);
                    }
                } catch (const std::exception&) {
                    explanation.status = ExplanationStatus::ServiceUnavailable;
                }
            } else {
                explanation.status = ExplanationStatus::NotConfigured;
            }
            output << '\n' << formatAiExplanation(explanation);
        }
        return output.str();
    }
    if (command == "performance") {
        std::vector<BacktestResult> results;
        results.reserve(2);
        bool storageFailed = false;
        for (const auto symbol : {MarketSymbol::BtcUsd, MarketSymbol::XauUsd}) {
            MarketDataSnapshot snapshot;
            snapshot.market = symbol;
            try {
                snapshot = marketDataService_->getSnapshot(symbol, "H4");
            } catch (const std::exception&) {
                // An empty replay produces an explicit insufficient-data result.
            }
            auto result = BacktestEngine::run(symbol, snapshot.candles, backtestSettings_);
            if (database_ != nullptr && result.status == BacktestStatus::Complete) {
                try {
                    database_->saveBacktestResult(result);
                } catch (const std::exception&) {
                    storageFailed = true;
                }
            }
            results.push_back(std::move(result));
        }
        auto message = formatBacktestPerformanceMessage(results);
        if (storageFailed) {
            message += "\n\nBacktest results could not be saved to local storage.";
        }
        return message;
    }
    if (command == "history") {
        if (database_ == nullptr) {
            return "SIMULATED BACKTEST TRADE HISTORY — local storage is unavailable.";
        }
        try {
            const auto signals = database_->loadRecentSignalScenarios(10);
            const auto totalSignals = database_->queryInt64("SELECT COUNT(*) FROM signal_scenarios");
            const auto trades = database_->loadRecentBacktestTrades(10);
            const auto total = database_->queryInt64("SELECT COUNT(*) FROM backtest_trades");
            return formatPaperSignalHistory(signals,
                       static_cast<std::size_t>(std::max<std::int64_t>(0, totalSignals)),
                       signalSettings_.timezone) + "\n" +
                   formatBacktestHistory(trades,
                       static_cast<std::size_t>(std::max<std::int64_t>(0, total)));
        } catch (const std::exception&) {
            return "SIMULATED BACKTEST TRADE HISTORY — local history could not be read.";
        }
    }
    if (command == "status") {
        return scheduler_ != nullptr ? scheduler_->statusMessage() : formatStatusMessage();
    }
    return "Unknown command. Use /help to see available commands.";
}

TelegramBot::TelegramBot(TelegramApiClient& apiClient, Logger& logger,
                         MarketDataService& marketDataService,
                         std::vector<std::int64_t> allowedUserIds,
                         long longPollTimeoutSeconds, std::int64_t initialOffset,
                         OffsetStore offsetStore, FinancialInsightService* insightService,
                         Database* database, BacktestSettings backtestSettings,
                         MonitoringScheduler* scheduler,
                         EconomicCalendarService* calendarService,
                         SignalCommandSettings signalSettings)
    : apiClient_(&apiClient), logger_(&logger), allowedUserIds_(std::move(allowedUserIds)),
      longPollTimeoutSeconds_(longPollTimeoutSeconds), nextOffset_(initialOffset),
      offsetStore_(std::move(offsetStore)),
      commandHandler_(marketDataService, insightService, database, backtestSettings, scheduler,
                      calendarService, std::move(signalSettings)) {
    if (longPollTimeoutSeconds_ <= 0) {
        throw AppError(ErrorCode::Configuration, "Telegram long-poll timeout must be positive");
    }
    if (nextOffset_ < 0) {
        throw AppError(ErrorCode::Configuration, "Telegram update offset must not be negative");
    }
    std::sort(allowedUserIds_.begin(), allowedUserIds_.end());
    allowedUserIds_.erase(std::unique(allowedUserIds_.begin(), allowedUserIds_.end()),
                          allowedUserIds_.end());
}

void TelegramBot::run(const std::function<bool()>& stopRequested) {
    auto backoff = std::chrono::seconds(1);
    while (!stopRequested || !stopRequested()) {
        try {
            pollOnce();
            backoff = std::chrono::seconds(1);
        } catch (const TelegramApiError& error) {
            if (!error.retryable()) {
                logger_->error("telegram.poll_stopped", {
                    {"api_error_code", error.apiErrorCode()},
                    {"http_status", error.httpStatus()},
                });
                throw;
            }
            const auto delay = error.retryAfterSeconds()
                                   ? std::chrono::seconds(*error.retryAfterSeconds())
                                   : backoff;
            logger_->warning("telegram.api_retry", {
                {"api_error_code", error.apiErrorCode()},
                {"http_status", error.httpStatus()},
                {"retry_seconds", delay.count()},
            });
            sleepInterruptibly(delay, stopRequested);
            backoff = nextBackoff(backoff);
        } catch (const AppError& error) {
            if (error.code() == ErrorCode::Configuration || error.code() == ErrorCode::Storage) {
                logger_->error("telegram.poll_stopped", {
                    {"error_code", errorCodeName(error.code())},
                });
                throw;
            }
            logger_->warning("telegram.connection_retry", {
                {"error_code", errorCodeName(error.code())},
                {"retry_seconds", backoff.count()},
            });
            sleepInterruptibly(backoff, stopRequested);
            backoff = nextBackoff(backoff);
        }
    }
}

void TelegramBot::pollOnce() {
    const auto updates = apiClient_->getUpdates(nextOffset_, longPollTimeoutSeconds_);
    for (const auto& update : updates) {
        if (update.updateId < nextOffset_) {
            continue;
        }
        handleUpdate(update);
        if (update.updateId >= nextOffset_) {
            if (update.updateId == std::numeric_limits<std::int64_t>::max()) {
                throw AppError(ErrorCode::Json, "Telegram update_id cannot be advanced safely");
            }
            const auto newOffset = update.updateId + 1;
            if (offsetStore_) {
                offsetStore_(newOffset);
            }
            nextOffset_ = newOffset;
        }
    }
}

void TelegramBot::handleUpdate(const TelegramUpdate& update) {
    if (!update.message || !update.message->privateChat || !update.message->userId ||
        !update.message->chatId ||
        !isAuthorized(*update.message->userId)) {
        // Do not reply to or disclose bot behavior to users outside the allowlist.
        return;
    }

    const auto response = commandHandler_.handle(update.message->text, update.message->userId);
    try {
        apiClient_->sendMessage(*update.message->chatId, response);
    } catch (const TelegramApiError& error) {
        if (error.retryable()) {
            throw;
        }
        // Permanent per-chat failures (for example, a blocked bot) must not pin
        // the update queue forever. Do not log message contents or API details.
        logger_->warning("telegram.reply_rejected", {
            {"api_error_code", error.apiErrorCode()},
            {"http_status", error.httpStatus()},
        });
    }
}

bool TelegramBot::isAuthorized(std::int64_t userId) const {
    return std::binary_search(allowedUserIds_.begin(), allowedUserIds_.end(), userId);
}

} // namespace trading_bot
