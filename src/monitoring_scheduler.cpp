#include "trading_bot/monitoring_scheduler.hpp"

#include "trading_bot/error.hpp"
#include "trading_bot/signal_engine.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>

namespace trading_bot {
namespace {

using Clock = std::chrono::system_clock;

std::int64_t toEpochMilliseconds(Clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

Clock::time_point fromEpochMilliseconds(std::int64_t value) {
    return Clock::time_point(std::chrono::milliseconds(value));
}

std::string formatLocalTime(Clock::time_point value, std::string_view timezone) {
    const auto raw = Clock::to_time_t(value);
    std::tm local{};
    if (localtime_r(&raw, &local) == nullptr) {
        return "unavailable";
    }
    std::ostringstream result;
    result << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << ' ' << timezone;
    return result.str();
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool isImportantHeadline(std::string_view headline) {
    static constexpr std::string_view terms[]{
        "bitcoin", "btc", "crypto", "digital asset", "spot etf", "exchange-traded fund",
        "gold", "xau", "federal reserve", "fed ", "interest rate", "rate cut", "rate hike",
        "inflation", "consumer price", "cpi", "pce", "fomc", "treasury", "yield", "sec "};
    const auto normalized = lower(std::string(headline));
    return std::any_of(std::begin(terms), std::end(terms), [&](std::string_view term) {
        return normalized.find(term) != std::string::npos;
    });
}

std::string boundedHeadline(std::string headline) {
    constexpr std::size_t maximumLength = 400;
    if (headline.size() > maximumLength) {
        headline.resize(maximumLength - 3);
        headline += "...";
    }
    return headline;
}

std::chrono::seconds retryBackoff(std::size_t attempts) {
    const auto exponent = std::min<std::size_t>(attempts, 8);
    return std::chrono::seconds(std::min<std::int64_t>(1LL << exponent, 300));
}

} // namespace

MonitoringScheduler::MonitoringScheduler(Database& database, MarketDataService& marketData,
                                         FinancialNewsService& news, TelegramApiClient& telegram,
                                         Logger& logger, std::vector<std::int64_t> allowedUserIds,
                                         MonitoringSettings settings)
    : database_(&database), marketData_(&marketData), news_(&news), telegram_(&telegram),
      logger_(&logger), allowedUserIds_(std::move(allowedUserIds)), settings_(std::move(settings)) {
    if (settings_.marketCheckInterval < std::chrono::seconds(60) ||
        settings_.newsCheckInterval < std::chrono::seconds(300) || settings_.timezone.empty()) {
        throw AppError(ErrorCode::Configuration, "Invalid monitoring scheduler settings");
    }
    if (allowedUserIds_.empty() ||
        std::any_of(allowedUserIds_.begin(), allowedUserIds_.end(), [](std::int64_t id) { return id <= 0; })) {
        throw AppError(ErrorCode::Configuration,
                       "Monitoring requires a non-empty allowlist of positive Telegram user IDs");
    }
    std::sort(allowedUserIds_.begin(), allowedUserIds_.end());
    allowedUserIds_.erase(std::unique(allowedUserIds_.begin(), allowedUserIds_.end()),
                          allowedUserIds_.end());

    if (const auto saved = database_->metadataInt64("monitor_last_market_check_ms")) {
        lastMarketCheck_ = fromEpochMilliseconds(*saved);
    }
    if (const auto saved = database_->metadataInt64("monitor_last_news_check_ms")) {
        lastNewsCheck_ = fromEpochMilliseconds(*saved);
    }
}

void MonitoringScheduler::run(std::stop_token stopToken) {
    {
        std::lock_guard lock(statusMutex_);
        running_ = true;
    }
    while (!stopToken.stop_requested()) {
        try {
            tick();
        } catch (const std::exception&) {
            recordFailure("scheduler", "tick_failed", Clock::now());
        } catch (...) {
            recordFailure("scheduler", "unknown_failure", Clock::now());
        }
        for (int step = 0; step < 10 && !stopToken.stop_requested(); ++step) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    std::lock_guard lock(statusMutex_);
    running_ = false;
}

void MonitoringScheduler::tick(Clock::time_point now) {
    std::optional<Clock::time_point> marketLast;
    std::optional<Clock::time_point> newsLast;
    {
        std::lock_guard lock(statusMutex_);
        marketLast = lastMarketCheck_;
        newsLast = lastNewsCheck_;
    }
    if (!marketLast || now < *marketLast || now - *marketLast >= settings_.marketCheckInterval) {
        checkMarkets(now);
    }
    if (!newsLast || now < *newsLast || now - *newsLast >= settings_.newsCheckInterval) {
        checkNews(now);
    }
    deliverPending(now);
}

void MonitoringScheduler::checkNow(Clock::time_point now) {
    checkMarkets(now);
    checkNews(now);
    deliverPending(now);
}

void MonitoringScheduler::checkMarkets(Clock::time_point now) {
    for (const auto market : {MarketSymbol::BtcUsd, MarketSymbol::XauUsd}) {
        try {
            const auto snapshot = marketData_->getSnapshot(market);
            const auto scenario = SignalEngine::evaluate(snapshot, now);
            const auto state = scenario.dataStatus == SignalDataStatus::Ready
                                   ? signalStateName(scenario.state)
                                   : std::string("WAIT");
            const auto marketName = marketSymbolName(market);
            std::string dedupeKey = "paper_signal:" + marketName + ":" + state + ":";
            if (scenario.candleTimestamp) {
                dedupeKey += std::to_string(toEpochMilliseconds(*scenario.candleTimestamp));
            } else {
                dedupeKey += std::to_string(toEpochMilliseconds(now));
            }
            const auto message = "EDUCATIONAL PAPER SIGNAL ALERT\n" + formatSignalScenario(scenario);
            const bool queued = database_->recordSignalTransition(
                marketName, state, dedupeKey, message, allowedUserIds_, now);
            if (queued) {
                logger_->info("scheduler.paper_signal_queued", {
                    {"market", marketName}, {"state", state},
                });
            }
        } catch (const std::exception&) {
            recordFailure("market", "market_data_or_analysis_failed", now);
            try {
                (void)database_->recordSignalTransition(marketSymbolName(market), "WAIT", "", "",
                                                        allowedUserIds_, now);
            } catch (const std::exception&) {
                recordFailure("storage", "signal_state_write_failed", now);
            }
        }
    }
    try {
        database_->setMetadataInt64("monitor_last_market_check_ms", toEpochMilliseconds(now));
        std::lock_guard lock(statusMutex_);
        lastMarketCheck_ = now;
    } catch (const std::exception&) {
        recordFailure("storage", "market_check_timestamp_failed", now);
    }
}

void MonitoringScheduler::checkNews(Clock::time_point now) {
    try {
        const auto snapshot = news_->getLatestNews(20, now);
        if (snapshot.articles.empty() && !snapshot.unavailableSources.empty()) {
            recordFailure("news", "all_feeds_unavailable", now);
        }
        for (const auto& article : snapshot.articles) {
            if (article.url.empty() || article.publishedAt > now ||
                now - article.publishedAt > std::chrono::hours(72) ||
                !isImportantHeadline(article.headline)) {
                continue;
            }
            std::ostringstream message;
            message << "IMPORTANT FINANCIAL NEWS — PUBLISHER REPORT\n"
                    << "Headline: " << boundedHeadline(article.headline) << '\n'
                    << "Source: " << article.source << '\n'
                    << "Published: " << formatLocalTime(article.publishedAt, settings_.timezone) << '\n'
                    << "Source link: " << article.url << '\n'
                    << "This is a publisher headline; implications are uncertain and not verified here.";
            (void)database_->enqueueAlert("news:" + article.url, "financial_news", message.str(),
                                          article.url, allowedUserIds_, now);
        }
        database_->setMetadataInt64("monitor_last_news_check_ms", toEpochMilliseconds(now));
        {
            std::lock_guard lock(statusMutex_);
            lastNewsCheck_ = now;
        }
    } catch (const std::exception&) {
        recordFailure("news", "news_fetch_or_storage_failed", now);
        try {
            database_->setMetadataInt64("monitor_last_news_check_ms", toEpochMilliseconds(now));
            std::lock_guard lock(statusMutex_);
            lastNewsCheck_ = now;
        } catch (const std::exception&) {
            recordFailure("storage", "news_check_timestamp_failed", now);
        }
    }
}

void MonitoringScheduler::deliverPending(Clock::time_point now) {
    std::vector<AlertDelivery> pending;
    try {
        pending = database_->loadDueAlertDeliveries(now, 100);
    } catch (const std::exception&) {
        recordFailure("storage", "alert_queue_read_failed", now);
        return;
    }
    for (const auto& delivery : pending) {
        if (!std::binary_search(allowedUserIds_.begin(), allowedUserIds_.end(), delivery.userId)) {
            try {
                database_->deferAlertDelivery(delivery.alertId, delivery.userId, now,
                                              "recipient_not_allowlisted", true);
            } catch (const std::exception&) {
                recordFailure("storage", "alert_failure_write_failed", now);
            }
            recordFailure("access", "recipient_not_allowlisted", now);
            continue;
        }
        if (const auto last = lastDeliveryByUser_.find(delivery.userId);
            last != lastDeliveryByUser_.end() && now < last->second + std::chrono::seconds(1)) {
            try {
                database_->deferAlertDelivery(delivery.alertId, delivery.userId,
                                              last->second + std::chrono::seconds(1),
                                              "chat_rate_limit", false);
            } catch (const std::exception&) {
                recordFailure("storage", "alert_retry_write_failed", now);
            }
            continue;
        }
        try {
            telegram_->sendMessage(delivery.userId, delivery.message);
            database_->markAlertDelivered(delivery.alertId, delivery.userId, now);
            lastDeliveryByUser_.insert_or_assign(delivery.userId, now);
        } catch (const TelegramApiError& error) {
            const bool isRateLimited = error.httpStatus() == 429 || error.apiErrorCode() == 429;
            if (isRateLimited) {
                const auto delay = error.retryAfterSeconds()
                    ? std::chrono::seconds(std::clamp(*error.retryAfterSeconds(), 1L, 86'400L))
                    : retryBackoff(delivery.attempts);
                lastDeliveryByUser_.insert_or_assign(
                    delivery.userId, now + delay - std::chrono::seconds(1));
                try {
                    database_->deferAlertDelivery(delivery.alertId, delivery.userId,
                                                  now + delay, "telegram_rate_limited", false);
                } catch (const std::exception&) {
                    recordFailure("storage", "alert_retry_write_failed", now);
                }
                recordFailure("telegram", "rate_limited", now);
            } else if (error.retryable()) {
                const auto delay = retryBackoff(delivery.attempts);
                lastDeliveryByUser_.insert_or_assign(
                    delivery.userId, now + delay - std::chrono::seconds(1));
                try {
                    database_->deferAlertDelivery(delivery.alertId, delivery.userId,
                                                  now + delay,
                                                  "telegram_transient_failure", false);
                } catch (const std::exception&) {
                    recordFailure("storage", "alert_retry_write_failed", now);
                }
                recordFailure("telegram", "transient_failure", now);
            } else {
                try {
                    database_->deferAlertDelivery(delivery.alertId, delivery.userId, now,
                                                  "telegram_rejected", true);
                } catch (const std::exception&) {
                    recordFailure("storage", "alert_failure_write_failed", now);
                }
                recordFailure("telegram", "permanent_rejection", now);
            }
        } catch (const std::exception&) {
            const auto delay = retryBackoff(delivery.attempts);
            lastDeliveryByUser_.insert_or_assign(
                delivery.userId, now + delay - std::chrono::seconds(1));
            try {
                database_->deferAlertDelivery(delivery.alertId, delivery.userId,
                                              now + delay,
                                              "telegram_transport_failure", false);
            } catch (const std::exception&) {
                recordFailure("storage", "alert_retry_write_failed", now);
            }
            recordFailure("telegram", "transport_failure", now);
        }
    }
}

void MonitoringScheduler::recordFailure(std::string_view component, std::string_view category,
                                        Clock::time_point now) noexcept {
    try {
        database_->recordSchedulerError(component, category, now);
    } catch (...) {
        // Health reporting must not terminate market monitoring on a logging failure.
    }
    try {
        logger_->warning("scheduler.error", {{"component", component}, {"category", category}});
    } catch (...) {
    }
}

std::string MonitoringScheduler::statusMessage() const {
    bool running = false;
    std::optional<Clock::time_point> marketLast;
    std::optional<Clock::time_point> newsLast;
    {
        std::lock_guard lock(statusMutex_);
        running = running_;
        marketLast = lastMarketCheck_;
        newsLast = lastNewsCheck_;
    }
    std::ostringstream status;
    status << "Bot health: " << (running ? "RUNNING" : "READY") << '\n'
           << "Monitoring timezone: " << settings_.timezone << '\n'
           << "Market checks: every " << settings_.marketCheckInterval.count() << " seconds; last: ";
    status << (marketLast ? formatLocalTime(*marketLast, settings_.timezone) : "not run yet") << '\n'
           << "News checks: every " << settings_.newsCheckInterval.count() << " seconds; last: ";
    status << (newsLast ? formatLocalTime(*newsLast, settings_.timezone) : "not run yet") << '\n'
           << "Trading: disabled; alerts are educational paper scenarios only.\n";
    try {
        status << "Pending alert deliveries: "
               << database_->queryInt64("SELECT COUNT(*) FROM scheduler_alert_deliveries WHERE state = 'PENDING'")
               << "; failed deliveries: "
               << database_->queryInt64("SELECT COUNT(*) FROM scheduler_alert_deliveries WHERE state = 'FAILED'")
               << '\n';
        const auto lastError = database_->lastSchedulerError();
        status << "Last scheduler error: " << lastError.value_or("none") << '\n'
               << "Storage: OK";
    } catch (const std::exception&) {
        status << "Local diagnostics unavailable (SQLite error).";
    }
    return status.str();
}

} // namespace trading_bot
