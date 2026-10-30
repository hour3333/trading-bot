#include "trading_bot/monitoring_scheduler.hpp"

#include "trading_bot/error.hpp"
#include "trading_bot/financial_insight.hpp"
#include "trading_bot/gold_market_analysis.hpp"
#include "trading_bot/signal_engine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <span>
#include <thread>
#include <utility>

namespace trading_bot {
namespace {

using Clock = std::chrono::system_clock;
constexpr auto kGoldM5AnalysisInterval = std::chrono::minutes(5);

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

std::string safeExternalLine(std::string_view text, std::size_t maximumLength) {
    std::string result;
    result.reserve(std::min(text.size(), maximumLength));
    for (const unsigned char character : text) {
        if (character < 0x20U || character == 0x7fU) {
            if (!result.empty() && result.back() != ' ') result.push_back(' ');
        } else {
            result.push_back(static_cast<char>(character));
        }
        if (result.size() >= maximumLength) break;
    }
    while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back()))) {
        result.pop_back();
    }
    return result;
}

std::uint64_t fnv1a(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string sectionValue(std::string_view text, std::string_view label,
                         std::span<const std::string_view> nextLabels) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    std::string loweredLabel(label);
    std::transform(loweredLabel.begin(), loweredLabel.end(), loweredLabel.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    const auto start = lowered.find(loweredLabel);
    if (start == std::string_view::npos) return {};
    auto valueStart = start + loweredLabel.size();
    if (valueStart < text.size() && (text[valueStart] == ' ' || text[valueStart] == '\t')) ++valueStart;
    auto end = text.size();
    for (const auto next : nextLabels) {
        std::string loweredNext(next);
        std::transform(loweredNext.begin(), loweredNext.end(), loweredNext.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        const auto found = lowered.find(loweredNext, valueStart);
        if (found != std::string_view::npos) end = std::min(end, found);
    }
    while (valueStart < end && (text[valueStart] == '\n' || text[valueStart] == '\r')) ++valueStart;
    while (end > valueStart && (text[end - 1] == '\n' || text[end - 1] == '\r')) --end;
    return std::string(text.substr(valueStart, end - valueStart));
}

std::string validNewsUrl(std::string_view url) {
    if (!url.starts_with("https://") || url.size() > 700 ||
        url.find_first_of("\r\n\t <>\"") != std::string_view::npos) return {};
    return std::string(url);
}

std::chrono::seconds retryBackoff(std::size_t attempts) {
    const auto exponent = std::min<std::size_t>(attempts, 8);
    return std::chrono::seconds(std::min<std::int64_t>(1LL << exponent, 300));
}

} // namespace

MonitoringScheduler::MonitoringScheduler(Database& database, MarketDataService& marketData,
                                         FinancialNewsService& news, TelegramApiClient& telegram,
                                         Logger& logger, std::vector<std::int64_t> allowedUserIds,
                                         MonitoringSettings settings,
                                         FinancialInsightService* insights)
    : database_(&database), marketData_(&marketData), news_(&news), insights_(insights),
      telegram_(&telegram), logger_(&logger), allowedUserIds_(std::move(allowedUserIds)),
      settings_(std::move(settings)) {
    if (settings_.marketCheckInterval < std::chrono::seconds(60) ||
        settings_.newsCheckInterval < std::chrono::seconds(300) ||
        settings_.newsNotificationsPerHour == 0 || settings_.newsNotificationsPerHour > 120 ||
        settings_.newsGeminiSummariesPerHour == 0 || settings_.newsGeminiSummariesPerHour > 120 ||
        settings_.timezone.empty()) {
        throw AppError(ErrorCode::Configuration, "Invalid monitoring scheduler settings");
    }
    if (settings_.signalMarket != MarketSymbol::XauUsd &&
        settings_.signalMarket != MarketSymbol::BtcUsd) {
        throw AppError(ErrorCode::Configuration, "Unsupported scheduler signal market");
    }
    if (settings_.signalTimeframe != "M5" && settings_.signalTimeframe != "M15" &&
        settings_.signalTimeframe != "H1" && settings_.signalTimeframe != "H4") {
        throw AppError(ErrorCode::Configuration, "Unsupported scheduler signal timeframe");
    }
    if (!std::isfinite(settings_.minimumRiskReward) || settings_.minimumRiskReward < 0.5 ||
        settings_.minimumRiskReward > 10.0 || settings_.signalAlertCooldown.count() < 0 ||
        (settings_.alertMode != "CONFIRMED_ONLY" && settings_.alertMode != "OFF") ||
        settings_.tradingMode != "PAPER_ONLY") {
        throw AppError(ErrorCode::Configuration, "Invalid paper-signal alert settings");
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
    if (const auto saved = database_->metadataInt64("monitor_last_gold_analysis_check_ms")) {
        lastGoldAnalysisCheck_ = fromEpochMilliseconds(*saved);
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
    std::optional<Clock::time_point> analysisLast;
    {
        std::lock_guard lock(statusMutex_);
        marketLast = lastMarketCheck_;
        newsLast = lastNewsCheck_;
        analysisLast = lastGoldAnalysisCheck_;
    }
    const bool marketDue = !marketLast || now < *marketLast ||
                           now - *marketLast >= settings_.marketCheckInterval;
    bool newsDue = !newsLast || now < *newsLast || now - *newsLast >= settings_.newsCheckInterval;
    try {
        if (const auto due = database_->metadataInt64("monitor_next_news_check_ms")) {
            newsDue = toEpochMilliseconds(now) >= *due;
        }
    } catch (const std::exception&) {
        recordFailure("storage", "news_schedule_read_failed", now);
    }
    const bool analysisDue = !analysisLast || now < *analysisLast ||
                             now - *analysisLast >= kGoldM5AnalysisInterval;
    const bool importantNewsDetected = newsDue ? checkNews(now) : false;
    if (marketDue) {
        checkMarkets(now, importantNewsDetected, analysisDue || importantNewsDetected);
    } else if (analysisDue || importantNewsDetected) {
        refreshGoldM5Analysis(now, importantNewsDetected);
    }
    deliverPending(now);
}

void MonitoringScheduler::checkNow(Clock::time_point now) {
    const bool importantNewsDetected = checkNews(now);
    checkMarkets(now, importantNewsDetected, true);
    deliverPending(now);
}

void MonitoringScheduler::checkMarkets(Clock::time_point now,
                                       bool importantNewsDetected,
                                       bool runGoldAnalysis) {
    const auto market = settings_.signalMarket;
    const auto marketName = marketSymbolName(market);
    SignalScenario scenario;
    std::optional<MarketDataSnapshot> reusableGoldM5Snapshot;
    try {
        const auto snapshot = marketData_->getSnapshot(market, settings_.signalTimeframe);
        if (market == MarketSymbol::XauUsd &&
            (settings_.signalTimeframe == "M5" || settings_.signalTimeframe == "5m") &&
            (runGoldAnalysis || importantNewsDetected)) {
            reusableGoldM5Snapshot = snapshot;
        }
        scenario = SignalEngine::evaluate(snapshot, settings_.signalTimeframe, now,
                                          settings_.minimumRiskReward);
        const bool validatedSetup = settings_.automaticSignalAnalysis &&
            settings_.alertMode == "CONFIRMED_ONLY" && settings_.tradingMode == "PAPER_ONLY" &&
            scenario.dataStatus == SignalDataStatus::Ready && scenario.state != SignalState::Wait &&
            !validateSignalScenario(scenario).has_value();
        const auto state = validatedSetup ? signalStateName(scenario.state) : std::string("WAIT");
        const auto candleMs = scenario.candleTimestamp
            ? toEpochMilliseconds(*scenario.candleTimestamp) : std::int64_t{0};
        database_->setMetadataText("monitor_signal_market", marketName);
        database_->setMetadataText("monitor_signal_timeframe", settings_.signalTimeframe);
        database_->setMetadataText("monitor_signal_data_status",
                                   signalDataStatusName(scenario.dataStatus));
        database_->setMetadataInt64("monitor_signal_last_check_ms", toEpochMilliseconds(now));
        if (candleMs > 0) {
            database_->setMetadataInt64("monitor_signal_last_candle_ms", candleMs);
        }

        if (validatedSetup && scenario.candleTimestamp) {
            const auto previousSavedCandle = database_->metadataInt64(
                "monitor_signal_last_saved_candle_ms");
            if (!previousSavedCandle || *previousSavedCandle != candleMs) {
                database_->saveSignalScenario(scenario);
                database_->setMetadataInt64("monitor_signal_last_saved_candle_ms", candleMs);
            }
        }

        const std::string dedupeKey = "paper_signal:" + marketName + ":" +
            settings_.signalTimeframe + ":" + state + ":" + std::to_string(candleMs);
        const std::string message = "EDUCATIONAL PAPER SIGNAL ALERT\n" +
            formatSignalScenario(scenario, settings_.timezone);
        const bool queued = database_->recordSignalTransition(
            marketName, state, dedupeKey, message, allowedUserIds_, now,
            settings_.signalAlertCooldown, settings_.allowDuplicateSignalAlerts);
        if (queued) {
            logger_->info("scheduler.paper_signal_queued", {
                {"market", marketName}, {"timeframe", settings_.signalTimeframe},
                {"state", state},
            });
        }
    } catch (const std::exception&) {
        recordFailure("market", "market_data_or_analysis_failed", now);
        try {
            (void)database_->recordSignalTransition(marketName, "WAIT", "", "",
                                                    allowedUserIds_, now);
            database_->setMetadataText("monitor_signal_data_status", "DATA UNAVAILABLE");
            database_->setMetadataInt64("monitor_signal_last_check_ms", toEpochMilliseconds(now));
        } catch (const std::exception&) {
            recordFailure("storage", "signal_state_write_failed", now);
        }
    }
    if (runGoldAnalysis || importantNewsDetected) {
        refreshGoldM5Analysis(now, importantNewsDetected,
                              reusableGoldM5Snapshot ? &*reusableGoldM5Snapshot : nullptr);
    }
    try {
        database_->setMetadataInt64("monitor_last_market_check_ms", toEpochMilliseconds(now));
        std::lock_guard lock(statusMutex_);
        lastMarketCheck_ = now;
    } catch (const std::exception&) {
        recordFailure("storage", "market_check_timestamp_failed", now);
    }
}

void MonitoringScheduler::refreshGoldM5Analysis(
    Clock::time_point now, bool importantNewsDetected,
    const MarketDataSnapshot* reusableSnapshot) {
    try {
        MarketDataSnapshot snapshot;
        if (reusableSnapshot != nullptr) {
            snapshot = *reusableSnapshot;
        } else {
            snapshot.market = MarketSymbol::XauUsd;
            snapshot.historyDescription =
                "No genuine completed XAU/USD M5 candles were returned by the configured provider.";
            try {
                snapshot = marketData_->getSnapshot(MarketSymbol::XauUsd, "M5");
            } catch (const std::exception&) {
                // Keep a fail-closed empty snapshot and report DATA UNAVAILABLE.
            }
        }

        NewsSnapshot newsSnapshot;
        bool haveCachedNews = false;
        {
            std::lock_guard lock(statusMutex_);
            if (latestNewsSnapshot_ && latestNewsSnapshot_->fetchedAt <= now &&
                now - latestNewsSnapshot_->fetchedAt <= std::chrono::minutes(5)) {
                newsSnapshot = *latestNewsSnapshot_;
                haveCachedNews = true;
            }
        }
        if (!haveCachedNews) {
            try {
                newsSnapshot = news_->getLatestNews(20, now);
                std::lock_guard lock(statusMutex_);
                latestNewsSnapshot_ = newsSnapshot;
            } catch (const std::exception&) {
                newsSnapshot.fetchedAt = now;
                newsSnapshot.unavailableSources.push_back("Configured financial news feeds");
            }
        }
        auto report = calculateGoldM5Analysis(
            snapshot, std::move(newsSnapshot), now, settings_.minimumRiskReward);
        const auto recipients = database_->analysisNotificationRecipients(allowedUserIds_);

        const auto previousCandleMs = database_->metadataInt64("monitor_gold_m5_analysis_candle_ms");
        const auto previousSemanticKey = database_->metadataText("monitor_gold_m5_analysis_semantic_key");
        const auto previousState = database_->metadataText("monitor_gold_m5_analysis_state");
        const auto currentCandleMs = report.technical.latestCandleEnd.time_since_epoch() >
                Clock::duration::zero()
            ? std::optional<std::int64_t>(toEpochMilliseconds(report.technical.latestCandleEnd))
            : std::nullopt;
        const bool newCompletedCandle = report.freshCompletedCandles && currentCandleMs &&
            (!previousCandleMs || *previousCandleMs != *currentCandleMs);
        std::ostringstream newsMaterial;
        if (importantNewsDetected) {
            for (const auto& article : report.news.articles) {
                const auto impact = classifyGoldNews(article);
                if (impact == NewsImpact::Low || !isGoldRelevantNews(article)) continue;
                newsMaterial << article.id << '|' << article.url << '|'
                             << article.headline << '|'
                             << toEpochMilliseconds(article.updatedAt.value_or(article.publishedAt))
                             << ';';
            }
        }
        std::string importantNewsKey;
        if (!newsMaterial.str().empty()) {
            std::ostringstream key;
            key << std::hex << fnv1a(newsMaterial.str());
            importantNewsKey = key.str();
        }

        AiExplanation explanation;
        const bool requestGemini = report.status == "COMPLETE" && insights_ != nullptr &&
                                   !recipients.empty() &&
                                   (newCompletedCandle || importantNewsDetected);
        if (requestGemini) {
            explanation = insights_->analyzeGoldM5(
                report.technical, report.scenario, report.news);
        } else if (insights_ == nullptr) {
            explanation.status = ExplanationStatus::NotConfigured;
        } else if (report.status != "COMPLETE") {
            explanation.status = ExplanationStatus::NoEvidence;
        }
        const std::string assessment = report.status != "COMPLETE" ? std::string{} :
            requestGemini ? formatAiExplanation(explanation) :
                "Gemini refresh waits for a new completed M5 candle or important verified gold news.";
        const std::string message = formatGoldM5AnalysisMessage(
            report, assessment, settings_.timezone);

        const bool validSetup = report.status == "COMPLETE" &&
            report.scenario.dataStatus == SignalDataStatus::Ready &&
            report.scenario.state != SignalState::Wait &&
            !validateSignalScenario(report.scenario).has_value();
        const std::string currentState = validSetup
            ? signalStateName(report.scenario.state) : "WAIT";
        const std::string currentTrend = report.technical.status == AnalysisStatus::Complete
            ? trendClassificationName(report.technical.trend) : "UNCONFIRMED";
        const std::string semanticKey = goldAnalysisChangeKey(report);
        const bool hasPrevious = previousSemanticKey.has_value();
        const bool changed = hasPrevious && *previousSemanticKey != semanticKey;
        const bool previousHadSetup = previousState &&
            (*previousState == "BUY SETUP" || *previousState == "SELL SETUP");
        // The stable analysis key covers status, trend, signal state,
        // confidence, risk, and deterministic indicator conflicts. Any key
        // change is therefore a material analysis change worth considering.
        const bool meaningfulChange = changed;
        const bool significant =
            (report.status == "COMPLETE" &&
             (importantNewsDetected || meaningfulChange || (!hasPrevious && validSetup))) ||
            (report.status != "COMPLETE" && previousHadSetup);
        const std::string_view importantNewsKeyView = importantNewsDetected
            ? std::string_view(importantNewsKey) : std::string_view{};
        const std::string notificationKey = goldAnalysisChangeKey(report, importantNewsKeyView);

        GoldAnalysisHistoryEntry history;
        history.analyzedAt = now;
        history.candleTimestamp = currentCandleMs
            ? std::optional<Clock::time_point>(report.technical.latestCandleEnd) : std::nullopt;
        history.status = report.status;
        history.trend = currentTrend;
        history.signalState = currentState;
        history.source = report.technical.source.empty()
            ? snapshot.historyDescription : report.technical.source;
        history.reason = report.reason;
        history.trigger = importantNewsDetected ? "important_news"
            : newCompletedCandle ? "new_completed_candle" : "scheduled_check";
        history.changeKey = notificationKey;
        history.message = message;
        database_->saveGoldAnalysisHistory(history);

        database_->setMetadataInt64("monitor_gold_m5_analysis_last_check_ms",
                                    toEpochMilliseconds(now));
        database_->setMetadataText("monitor_gold_m5_analysis_status", report.status);
        database_->setMetadataText("monitor_gold_m5_analysis_state", currentState);
        database_->setMetadataText("monitor_gold_m5_analysis_trend", currentTrend);
        database_->setMetadataText("monitor_gold_m5_analysis_semantic_key", semanticKey);
        if (currentCandleMs) {
            database_->setMetadataInt64("monitor_gold_m5_analysis_candle_ms", *currentCandleMs);
        }
        if (!importantNewsKey.empty()) {
            database_->setMetadataText("monitor_gold_m5_analysis_news_key", importantNewsKey);
        }

        if (significant && !recipients.empty()) {
            const auto alertMessage = "🔔 SIGNIFICANT GOLD ANALYSIS UPDATE\n" + message;
            (void)database_->enqueueAlert(notificationKey, "gold_analysis", alertMessage, "",
                                          recipients, now);
        }
        {
            std::lock_guard lock(statusMutex_);
            lastGoldAnalysisCheck_ = now;
        }
        database_->setMetadataInt64("monitor_last_gold_analysis_check_ms",
                                    toEpochMilliseconds(now));
    } catch (const std::exception&) {
        recordFailure("gold_analysis", "analysis_refresh_or_storage_failed", now);
        try {
            database_->setMetadataInt64("monitor_last_gold_analysis_check_ms",
                                        toEpochMilliseconds(now));
            std::lock_guard lock(statusMutex_);
            lastGoldAnalysisCheck_ = now;
        } catch (const std::exception&) {
            recordFailure("storage", "gold_analysis_schedule_write_failed", now);
        }
    }
}

bool MonitoringScheduler::checkNews(Clock::time_point now) {
    bool importantNewsDetected = false;
    std::optional<Clock::time_point> previousPoll;
    {
        std::lock_guard lock(statusMutex_);
        previousPoll = lastNewsCheck_;
    }
    try {
        const auto snapshot = news_->getLatestNews(20, now);
        {
            std::lock_guard lock(statusMutex_);
            auto fetchedSnapshot = snapshot;
            // The scheduler observed this successful fetch at `now`; normalize
            // a missing adapter timestamp to that local observation time.
            if (fetchedSnapshot.fetchedAt.time_since_epoch() <= Clock::duration::zero()) {
                fetchedSnapshot.fetchedAt = now;
            }
            latestNewsSnapshot_ = std::move(fetchedSnapshot);
        }
        const bool baselineInitialized =
            database_->metadataInt64("monitor_news_initialized").value_or(0) == 1;
        const auto baselineTimeMs = database_->metadataInt64("monitor_news_baseline_ms");
        const bool delayedSnapshot = !snapshot.delayedSources.empty();
        const bool failedSnapshot = snapshot.articles.empty() &&
                                    !snapshot.unavailableSources.empty() &&
                                    snapshot.delayedSources.empty();
        if (failedSnapshot) recordFailure("news", "all_feeds_unavailable", now);
        if (delayedSnapshot) recordFailure("news", "using_delayed_feed_cache", now);

        for (const auto& article : snapshot.articles) {
            const auto url = validNewsUrl(article.url);
            const auto activityAt = article.updatedAt.value_or(article.publishedAt);
            if (url.empty() || article.headline.empty() || article.source.empty() ||
                article.publishedAt.time_since_epoch() <= Clock::duration::zero() ||
                article.publishedAt > now + std::chrono::minutes(5) ||
                (article.updatedAt && *article.updatedAt < article.publishedAt) ||
                activityAt > now + std::chrono::minutes(5) ||
                now - activityAt > std::chrono::hours(72)) {
                continue;
            }
            if (delayedSnapshot && baselineInitialized) continue;
            const auto fallbackKey = article.id.empty() ? url : article.id;
            const std::string storyKey = "gold-news-story:" + std::to_string(fnv1a(fallbackKey));
            const bool predatesBaseline = baselineTimeMs &&
                toEpochMilliseconds(activityAt) < *baselineTimeMs;
            const bool predatesRestart = activityAt < processStartedAt_;
            const bool arrivedSincePreviousPoll = !previousPoll || now < *previousPoll ||
                                                  activityAt > *previousPoll;
            const auto impact = classifyGoldNews(article);
            const bool eligibleImpact = impact != NewsImpact::Low && isGoldRelevantNews(article);
            // Persist stale/backfilled and low-impact entries without alerting, so they
            // cannot turn into backlog on a later poll or process restart.
            const bool baselineOnly = !baselineInitialized || predatesBaseline || predatesRestart ||
                                      !arrivedSincePreviousPoll || !eligibleImpact;
            const auto story = database_->recordNewsStory(
                storyKey, article.id, url, safeExternalLine(article.headline, 300),
                safeExternalLine(article.source, 120), article.publishedAt, now, baselineOnly);
            if (!baselineOnly && story.shouldNotify && eligibleImpact) {
                importantNewsDetected = true;
            }
            if (baselineOnly || !story.shouldNotify) continue;

            const auto impactName = newsImpactName(impact);
            std::ostringstream dedupe;
            dedupe << "gold-news:" << std::hex << fnv1a(story.storyKey)
                   << ':' << story.revision;
            const auto recipients = database_->eligibleNewsRecipients(
                impactName, allowedUserIds_, now, settings_.newsNotificationsPerHour);
            if (recipients.empty()) {
                (void)database_->enqueueNewsAlert(
                    dedupe.str(), impactName, story.storyKey, story.revision,
                    "News alert suppressed by the user's saved preferences or hourly limit.",
                    url, {}, now, settings_.newsNotificationsPerHour);
                continue;
            }

            std::string facts;
            std::string whyGold;
            std::string possibleImpact;
            std::string uncertainty;
            if (insights_ != nullptr) {
                try {
                    const auto explanation = insights_->explainGoldNews(article);
                    if (explanation.status == ExplanationStatus::Available &&
                        !explanation.text.empty()) {
                        static constexpr std::string_view factNext[]{
                            "Why it matters for gold:", "Potential Gold Impact:", "Uncertainty:"};
                        static constexpr std::string_view whyNext[]{
                            "Potential Gold Impact:", "Uncertainty:"};
                        static constexpr std::string_view impactNext[]{"Uncertainty:"};
                        facts = sectionValue(explanation.text, "Facts:", factNext);
                        whyGold = sectionValue(explanation.text, "Why it matters for gold:", whyNext);
                        possibleImpact = sectionValue(
                            explanation.text, "Potential Gold Impact:", impactNext);
                        uncertainty = sectionValue(
                            explanation.text, "Uncertainty:", std::span<const std::string_view>{});
                    }
                } catch (const std::exception&) {
                    // The sourced headline and URL remain useful without optional Gemini context.
                }
            }
            if (facts.empty()) {
                facts = "Gemini commentary unavailable; this is a sourced publisher headline only.";
            }
            if (whyGold.empty()) {
                whyGold = "The headline may relate to gold through macroeconomic or risk sentiment channels; details are unavailable.";
            }
            if (possibleImpact.empty()) {
                possibleImpact = "Uncertain — the headline alone is insufficient to establish a market direction.";
            }
            if (uncertainty.empty()) {
                uncertainty = "Publisher headline only; implications are speculative and are not independently verified.";
            }

            std::ostringstream message;
            message << "🔔 GOLD MARKET NEWS\n"
                    << "Impact: " << impactName << '\n'
                    << "Market: XAU/USD\n"
                    << "Headline: " << safeExternalLine(article.headline, 300) << '\n'
                    << "Published: " << formatLocalTime(article.publishedAt, settings_.timezone) << '\n';
            if (article.updatedAt && *article.updatedAt > article.publishedAt) {
                message << "Updated: " << formatLocalTime(*article.updatedAt, settings_.timezone) << '\n';
            }
            message << "Detected: " << formatLocalTime(now, settings_.timezone) << '\n'
                    << "Source: " << safeExternalLine(article.source, 120) << '\n'
                    << "AI Summary:\nFacts: " << facts << '\n'
                    << "Why it matters for gold: " << whyGold << '\n'
                    << "Uncertainty: " << uncertainty << '\n'
                    << "Potential Gold Impact:\n" << possibleImpact << '\n'
                    << "Source Link: " << url << '\n'
                    << "Publisher report only; this is not a confirmed trading signal.";
            (void)database_->enqueueNewsAlert(
                dedupe.str(), impactName, story.storyKey, story.revision, message.str(), url,
                allowedUserIds_, now, settings_.newsNotificationsPerHour);
        }

        if (!baselineInitialized &&
            (snapshot.unavailableSources.empty() || !snapshot.articles.empty())) {
            database_->setMetadataInt64("monitor_news_baseline_ms", toEpochMilliseconds(now));
            database_->setMetadataInt64("monitor_news_initialized", 1);
        }

        const auto failures = failedSnapshot
            ? std::min<std::int64_t>(database_->metadataInt64("monitor_news_failure_streak").value_or(0) + 1, 8)
            : std::int64_t{0};
        database_->setMetadataInt64("monitor_news_failure_streak", failures);
        std::chrono::seconds nextDelay = settings_.newsCheckInterval;
        if (failedSnapshot) {
            const auto exponent = static_cast<unsigned>(std::min<std::int64_t>(failures - 1, 3));
            nextDelay = std::chrono::seconds(std::min<std::int64_t>(60LL << exponent, 300));
        }
        database_->setMetadataInt64("monitor_next_news_check_ms",
                                    toEpochMilliseconds(now + nextDelay));
        database_->setMetadataInt64("monitor_last_news_check_ms", toEpochMilliseconds(now));
        {
            std::lock_guard lock(statusMutex_);
            lastNewsCheck_ = now;
        }
        return importantNewsDetected;
    } catch (const std::exception&) {
        recordFailure("news", "news_fetch_or_storage_failed", now);
        try {
            const auto failures = std::min<std::int64_t>(
                database_->metadataInt64("monitor_news_failure_streak").value_or(0) + 1, 8);
            database_->setMetadataInt64("monitor_news_failure_streak", failures);
            const auto exponent = static_cast<unsigned>(std::min<std::int64_t>(failures - 1, 3));
            const auto delay = std::chrono::seconds(std::min<std::int64_t>(60LL << exponent, 300));
            database_->setMetadataInt64("monitor_next_news_check_ms",
                                        toEpochMilliseconds(now + delay));
            database_->setMetadataInt64("monitor_last_news_check_ms", toEpochMilliseconds(now));
            std::lock_guard lock(statusMutex_);
            lastNewsCheck_ = now;
        } catch (const std::exception&) {
            recordFailure("storage", "news_check_timestamp_failed", now);
        }
        return false;
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
    std::optional<Clock::time_point> analysisLast;
    {
        std::lock_guard lock(statusMutex_);
        running = running_;
        marketLast = lastMarketCheck_;
        newsLast = lastNewsCheck_;
        analysisLast = lastGoldAnalysisCheck_;
    }
    std::ostringstream status;
    status << "Bot health: " << (running ? "RUNNING" : "READY") << '\n'
           << "Monitoring timezone: " << settings_.timezone << '\n'
           << "Paper signal: " << marketSymbolName(settings_.signalMarket) << ' '
           << settings_.signalTimeframe << "; mode " << settings_.alertMode
           << "; trading " << settings_.tradingMode << '\n'
           << "Market checks: every " << settings_.marketCheckInterval.count() << " seconds; last: ";
    status << (marketLast ? formatLocalTime(*marketLast, settings_.timezone) : "not run yet") << '\n'
           << "News checks: every " << settings_.newsCheckInterval.count() << " seconds; last: ";
    status << (newsLast ? formatLocalTime(*newsLast, settings_.timezone) : "not run yet") << '\n'
           << "Gold M5 analysis checks: every " << kGoldM5AnalysisInterval.count()
           << " seconds; last: "
           << (analysisLast ? formatLocalTime(*analysisLast, settings_.timezone) : "not run yet") << '\n'
           << "Trading: disabled; alerts are educational paper scenarios only.\n";
    try {
        status << "Pending alert deliveries: "
               << database_->queryInt64("SELECT COUNT(*) FROM scheduler_alert_deliveries WHERE state = 'PENDING'")
               << "; failed deliveries: "
               << database_->queryInt64("SELECT COUNT(*) FROM scheduler_alert_deliveries WHERE state = 'FAILED'")
               << '\n';
        const auto lastError = database_->lastSchedulerError();
        const auto signalStatus = database_->metadataText("monitor_signal_data_status");
        const auto signalCheck = database_->metadataInt64("monitor_signal_last_check_ms");
        const auto candle = database_->metadataInt64("monitor_signal_last_candle_ms");
        status << "Signal data freshness: " << signalStatus.value_or("not checked yet")
               << "; checked: " << (signalCheck
                    ? formatLocalTime(fromEpochMilliseconds(*signalCheck), settings_.timezone)
                    : "not checked yet")
               << "; last completed candle: " << (candle
                    ? formatLocalTime(fromEpochMilliseconds(*candle), settings_.timezone)
                    : "DATA UNAVAILABLE") << '\n'
               << "Last scheduler error: " << lastError.value_or("none") << '\n'
               << "Storage: OK";
        status << "\nGold news stories tracked: "
               << database_->queryInt64("SELECT COUNT(*) FROM news_story_state")
               << "; per-user rate limit: " << settings_.newsNotificationsPerHour << "/hour";
    } catch (const std::exception&) {
        status << "Local diagnostics unavailable (SQLite error).";
    }
    return status.str();
}

std::string MonitoringScheduler::newsStatusMessage(std::int64_t userId) const {
    if (userId <= 0 || !std::binary_search(allowedUserIds_.begin(), allowedUserIds_.end(), userId)) {
        return "News settings are available only in an authorized private chat.";
    }
    try {
        const auto preference = database_->newsNotificationPreference(userId);
        std::ostringstream output;
        output << "GOLD NEWS NOTIFICATION SETTINGS\n"
               << "Automatic notifications: " << (preference.enabled ? "ON" : "OFF") << '\n'
               << "Impact filter: " << (preference.highImpactOnly
                    ? "HIGH only" : "HIGH and MEDIUM") << '\n'
               << "Polling interval: " << settings_.newsCheckInterval.count() << " seconds\n"
               << "Rate limit: " << settings_.newsNotificationsPerHour
               << " notifications per hour per user\n"
               << "Gemini summary cap: " << settings_.newsGeminiSummariesPerHour
               << " automatic summaries per hour\n"
               << "News impact is informational and does not confirm a trading signal.";
        return output.str();
    } catch (const std::exception&) {
        return "Gold news settings could not be read from local storage.";
    }
}

std::string MonitoringScheduler::analysisStatusMessage(std::int64_t userId) const {
    if (userId <= 0 || !std::binary_search(allowedUserIds_.begin(), allowedUserIds_.end(), userId)) {
        return "Analysis settings are available only in an authorized private chat.";
    }
    try {
        const auto preference = database_->analysisNotificationPreference(userId);
        const auto latest = database_->latestGoldAnalysisHistory();
        std::ostringstream output;
        output << "GOLD M5 ANALYSIS STATUS\n"
               << "Analysis notifications: " << (preference.enabled ? "ON" : "OFF") << '\n'
               << "Refresh interval: " << kGoldM5AnalysisInterval.count() << " seconds\n";
        if (!latest) {
            output << "Last update: not run yet\n"
                   << "Data freshness: no analysis record is available.";
            if (isGoldWeeklyClosureExpected(Clock::now())) {
                output << "\nMarket session: weekend closure is expected for many gold venues; exact hours vary by provider.";
            }
            return output.str();
        }
        output << "Last update: " << formatLocalTime(latest->analyzedAt, settings_.timezone) << '\n'
               << "Last data status: " << latest->status << '\n'
               << "Last trend / scenario: " << latest->trend << " / " << latest->signalState << '\n'
               << "Data source: " << (latest->source.empty() ? "DATA UNAVAILABLE" : latest->source) << '\n'
               << "Data freshness: ";
        const auto now = Clock::now();
        if (!latest->candleTimestamp) {
            output << "DATA UNAVAILABLE (no completed M5 candle timestamp)";
        } else if (*latest->candleTimestamp > now) {
            output << "INVALID (future candle timestamp)";
        } else {
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - *latest->candleTimestamp);
            output << (latest->status == "COMPLETE" && age <= std::chrono::minutes(10)
                            ? "FRESH" : "STALE")
                   << " (completed candle "
                   << formatLocalTime(*latest->candleTimestamp, settings_.timezone) << ')';
        }
        if (isGoldWeeklyClosureExpected(now)) {
            output << "\nMarket session: weekend closure is expected for many gold venues; exact hours vary by provider.";
        }
        output << "\nPaper trading only; analysis notifications do not place orders.";
        return output.str();
    } catch (const std::exception&) {
        return "Gold M5 analysis status could not be read from local storage.";
    }
}

} // namespace trading_bot
