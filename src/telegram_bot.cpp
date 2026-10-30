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

#include <algorithm>
#include <chrono>
#include <cctype>
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

} // namespace

TelegramCommandHandler::TelegramCommandHandler(MarketDataService& marketDataService,
                                               FinancialInsightService* insightService,
                                               Database* database,
                                               BacktestSettings backtestSettings,
                                               MonitoringScheduler* scheduler,
                                               EconomicCalendarService* calendarService)
    : marketDataService_(&marketDataService), insightService_(insightService),
      database_(database), backtestSettings_(backtestSettings), scheduler_(scheduler),
      calendarService_(calendarService) {}

std::string TelegramCommandHandler::handle(std::string_view messageText) const {
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
    if (command == "analysis") {
        std::vector<TechnicalAnalysisResult> analyses;
        analyses.reserve(2);
        for (const auto symbol : {MarketSymbol::BtcUsd, MarketSymbol::XauUsd}) {
            try {
                const auto snapshot = marketDataService_->getSnapshot(symbol);
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
            const auto news = insightService_->getLatestNews();
            const auto explanation = insightService_->explainNews(news);
            return formatNewsMessage(news, 3, 2500) + "\n" +
                   formatAiExplanation(explanation);
        } catch (const std::exception&) {
            NewsSnapshot unavailable;
            return formatNewsMessage(unavailable) + "\n" +
                   "AI commentary unavailable: news retrieval failed.";
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
    if (command == "signal") {
        std::ostringstream output;
        for (std::size_t index = 0; index < 2; ++index) {
            const auto symbol = index == 0 ? MarketSymbol::BtcUsd : MarketSymbol::XauUsd;
            MarketDataSnapshot snapshot;
            snapshot.market = symbol;
            snapshot.historyDescription = "Historical OHLC";
            try {
                snapshot = marketDataService_->getSnapshot(symbol);
            } catch (const std::exception&) {
                // Empty snapshot is evaluated as WAIT and saved without levels.
            }
            auto scenario = SignalEngine::evaluate(snapshot);
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
            output << formatSignalScenario(scenario);
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
                    explanation = insightService_->explainSignal(scenario);
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
                snapshot = marketDataService_->getSnapshot(symbol);
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
            const auto trades = database_->loadRecentBacktestTrades(10);
            const auto total = database_->queryInt64("SELECT COUNT(*) FROM backtest_trades");
            return formatBacktestHistory(trades, static_cast<std::size_t>(std::max<std::int64_t>(0, total)));
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
                         EconomicCalendarService* calendarService)
    : apiClient_(&apiClient), logger_(&logger), allowedUserIds_(std::move(allowedUserIds)),
      longPollTimeoutSeconds_(longPollTimeoutSeconds), nextOffset_(initialOffset),
      offsetStore_(std::move(offsetStore)),
      commandHandler_(marketDataService, insightService, database, backtestSettings, scheduler,
                      calendarService) {
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

    const auto response = commandHandler_.handle(update.message->text);
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
