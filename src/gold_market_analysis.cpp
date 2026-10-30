#include "trading_bot/gold_market_analysis.hpp"

#include "trading_bot/signal_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace trading_bot {
namespace {

using Clock = std::chrono::system_clock;

std::string localTimestamp(Clock::time_point timestamp, std::string_view timezone) {
    const auto raw = Clock::to_time_t(timestamp);
    std::tm local{};
    if (localtime_r(&raw, &local) == nullptr) return "DATA UNAVAILABLE";
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << ' ' << timezone;
    return output.str();
}

std::string safeLine(std::string_view value, std::size_t maxBytes) {
    std::string output;
    output.reserve(std::min(value.size(), maxBytes));
    for (const unsigned char byte : value) {
        if (byte < 0x20U || byte == 0x7fU) {
            if (!output.empty() && output.back() != ' ') output.push_back(' ');
        } else {
            output.push_back(static_cast<char>(byte));
        }
        if (output.size() >= maxBytes) break;
    }
    while (!output.empty() && output.back() == ' ') output.pop_back();
    return output;
}

std::string safeBlock(std::string_view value, std::size_t maxBytes) {
    std::string output;
    output.reserve(std::min(value.size(), maxBytes));
    for (const unsigned char byte : value) {
        if (byte == '\n') {
            if (output.empty() || output.back() != '\n') output.push_back('\n');
        } else if (byte == '\r' || byte == '\t') {
            if (!output.empty() && output.back() != ' ') output.push_back(' ');
        } else if (byte >= 0x20U && byte != 0x7fU) {
            output.push_back(static_cast<char>(byte));
        }
        if (output.size() >= maxBytes) break;
    }
    return output;
}

std::string price(const std::optional<double>& value) {
    if (!value || !std::isfinite(*value) || *value <= 0.0) return "N/A";
    std::ostringstream output;
    output << '$' << std::fixed << std::setprecision(2) << *value;
    return output.str();
}

std::string number(const std::optional<double>& value, int precision = 2) {
    if (!value || !std::isfinite(*value)) return "N/A";
    std::ostringstream output;
    output << std::fixed << std::setprecision(precision) << *value;
    return output.str();
}

std::string stateName(const SignalScenario& scenario, bool validated) {
    if (!validated) return "WAIT";
    return signalStateName(scenario.state);
}

std::string trendName(const TechnicalAnalysisResult& analysis) {
    if (analysis.status != AnalysisStatus::Complete) return "Unconfirmed";
    if (analysis.trend == TrendClassification::Bullish) return "Bullish";
    if (analysis.trend == TrendClassification::Bearish) return "Bearish";
    return "Sideways";
}

std::string truncateUtf8(std::string text, std::size_t maximumBytes) {
    if (text.size() <= maximumBytes) return text;
    text.resize(maximumBytes);
    while (!text.empty() &&
           (static_cast<unsigned char>(text.back()) & 0xc0U) == 0x80U) {
        text.pop_back();
    }
    if (!text.empty()) {
        const auto lead = static_cast<unsigned char>(text.back());
        if ((lead & 0x80U) != 0U) text.pop_back();
    }
    return text;
}

std::uint64_t fnv1a(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace

bool isGoldWeeklyClosureExpected(Clock::time_point timestamp) {
    const auto raw = Clock::to_time_t(timestamp);
    std::tm utc{};
    if (gmtime_r(&raw, &utc) == nullptr) return false;
    // Spot-gold/CFD hours vary by provider and daylight-saving time. This is a
    // conservative weekend hint, not an exchange calendar or a live-status claim.
    return utc.tm_wday == 6 || (utc.tm_wday == 0 && utc.tm_hour < 22) ||
           (utc.tm_wday == 5 && utc.tm_hour >= 22);
}

GoldMarketAnalysisReport calculateGoldM5Analysis(
    const MarketDataSnapshot& market, NewsSnapshot news, Clock::time_point asOf,
    double minimumRiskReward) {
    GoldMarketAnalysisReport report;
    report.analyzedAt = asOf;
    report.news = std::move(news);
    report.expectedWeeklyClosure = isGoldWeeklyClosureExpected(asOf);

    std::erase_if(report.news.articles, [&](const NewsArticle& article) {
        const auto activityAt = article.updatedAt.value_or(article.publishedAt);
        return article.headline.empty() || article.source.empty() ||
               !article.url.starts_with("https://") ||
               article.url.find_first_of("\r\n\t <>\"") != std::string::npos ||
               article.publishedAt.time_since_epoch() <= Clock::duration::zero() ||
               article.publishedAt > asOf + std::chrono::minutes(5) ||
               (article.updatedAt && *article.updatedAt < article.publishedAt) ||
               activityAt > asOf + std::chrono::minutes(5) ||
               asOf - activityAt > std::chrono::hours(72) || !isGoldRelevantNews(article);
    });
    std::stable_sort(report.news.articles.begin(), report.news.articles.end(),
        [](const NewsArticle& left, const NewsArticle& right) {
            return left.updatedAt.value_or(left.publishedAt) >
                   right.updatedAt.value_or(right.publishedAt);
        });
    std::unordered_set<std::string> seenUrls;
    std::erase_if(report.news.articles, [&](const NewsArticle& article) {
        return !seenUrls.insert(article.url).second;
    });
    if (report.news.articles.size() > 3) report.news.articles.resize(3);

    if (market.market != MarketSymbol::XauUsd) {
        report.technical = TechnicalAnalysis::analyze(
            MarketSymbol::XauUsd, std::span<const OhlcCandle>{}, asOf);
        report.scenario = SignalEngine::evaluate(market, "M5", asOf, minimumRiskReward);
        report.status = "DATA UNAVAILABLE";
        report.reason = "The returned market snapshot is not XAU/USD.";
        return report;
    }

    report.technical = TechnicalAnalysis::analyze(
        MarketSymbol::XauUsd, market.candles, asOf);
    report.scenario = SignalEngine::evaluate(market, "M5", asOf, minimumRiskReward);
    if (market.candles.empty()) {
        report.status = "DATA UNAVAILABLE";
        report.reason = market.historyDescription.empty()
            ? "No genuine completed XAU/USD M5 OHLC candles were returned by the configured provider."
            : market.historyDescription;
        if (report.expectedWeeklyClosure) {
            report.reason += "; weekend closure is expected for many gold venues, but provider hours vary";
        }
        return report;
    }
    if (std::any_of(market.candles.begin(), market.candles.end(),
                    [](const OhlcCandle& candle) { return candle.interval != "5m"; })) {
        report.status = "DATA UNAVAILABLE";
        report.reason = "The configured source did not return genuine fixed-interval M5 candles.";
        return report;
    }
    if (report.technical.status == AnalysisStatus::InvalidData) {
        report.status = "DATA UNAVAILABLE";
        report.reason = report.technical.reason;
        return report;
    }
    if (report.technical.status != AnalysisStatus::Complete) {
        report.status = "INSUFFICIENT DATA";
        report.reason = report.technical.reason;
        return report;
    }

    constexpr auto interval = std::chrono::minutes(5);
    constexpr auto maxCandleAge = interval * 2;
    constexpr auto maxFetchAge = std::chrono::minutes(10);
    const auto& latest = market.candles.back();
    if (latest.interval != "5m" || latest.delayed || latest.endTime > asOf ||
        asOf - latest.endTime > maxCandleAge ||
        latest.fetchedAt.time_since_epoch() <= Clock::duration::zero() ||
        latest.fetchedAt > asOf + std::chrono::seconds(30) ||
        asOf - latest.fetchedAt > maxFetchAge) {
        report.technical.status = AnalysisStatus::InsufficientData;
        report.technical.reason = "The latest completed M5 candle is stale, delayed, or not a fixed five-minute bar.";
        report.status = "STALE DATA";
        report.reason = report.technical.reason;
        if (report.expectedWeeklyClosure) {
            report.reason += " Weekend closure is expected for many gold venues; provider hours vary.";
        }
        return report;
    }

    report.freshCompletedCandles = true;
    report.status = "COMPLETE";
    report.reason = report.scenario.dataStatus == SignalDataStatus::Ready
        ? "Completed M5 history and current quote passed deterministic validation."
        : report.scenario.reasons.empty()
            ? "M5 indicators are available; a confirmed paper setup is not established."
            : report.scenario.reasons.back();
    return report;
}

std::string formatGoldM5AnalysisMessage(const GoldMarketAnalysisReport& report,
                                       std::string_view geminiAssessment,
                                       std::string_view timezone) {
    const auto analysisTimestamp = localTimestamp(report.analyzedAt, timezone);
    const bool validSetup = report.status == "COMPLETE" &&
        report.scenario.dataStatus == SignalDataStatus::Ready &&
        report.scenario.state != SignalState::Wait &&
        !validateSignalScenario(report.scenario).has_value();
    const auto state = stateName(report.scenario, validSetup);
    std::ostringstream output;
    output << "📊 XAU/USD — GOLD ANALYSIS\n"
           << "Date: " << (analysisTimestamp == "DATA UNAVAILABLE"
                ? analysisTimestamp : analysisTimestamp.substr(0, 10)) << '\n'
           << "Time: " << (analysisTimestamp == "DATA UNAVAILABLE"
                ? analysisTimestamp : analysisTimestamp.substr(11)) << '\n'
           << "Timeframe: M5\n"
           << "Trend: " << trendName(report.technical) << '\n'
           << "Status: " << state << "\n\n"
           << "Parameter | Assessment\n"
           << "Entry zone to watch | "
           << (validSetup ? price(report.scenario.entryZoneLow) + "–" +
                              price(report.scenario.entryZoneHigh) : "N/A") << '\n'
           << "Stop loss | " << (validSetup ? price(report.scenario.stopLoss) : "N/A") << '\n'
           << "Take Profit 1 | " << (validSetup ? price(report.scenario.takeProfit1) : "N/A") << '\n'
           << "Take Profit 2 | " << (validSetup ? price(report.scenario.takeProfit2) : "N/A") << '\n'
           << "Risk/reward | " << (validSetup
                ? "TP1 1:" + number(report.scenario.riskReward1) + ", TP2 1:" +
                    number(report.scenario.riskReward2) : "N/A") << '\n'
           << "Confidence | " << signalConfidenceName(report.scenario.confidence)
           << " — " << safeLine(report.scenario.confidenceReason, 240) << '\n'
           << "Risk | " << signalRiskLevelName(report.scenario.riskLevel)
           << " — " << safeLine(report.scenario.riskReason, 240) << "\n\n"
           << "Technical Analysis:\n";

    if (report.technical.status == AnalysisStatus::Complete && report.freshCompletedCandles) {
        output << "Latest completed close: " << price(report.technical.latestClose) << '\n'
               << "EMA 20 / EMA 50: " << number(report.technical.ema20) << " / "
               << number(report.technical.ema50) << '\n'
               << "RSI 14: " << number(report.technical.rsi) << '\n'
               << "MACD 12/26/9: " << (report.technical.macd
                    ? number(report.technical.macd->line) + " / signal " +
                      number(report.technical.macd->signal) + " / histogram " +
                      number(report.technical.macd->histogram) : "N/A") << '\n'
               << "ATR 14: " << number(report.technical.atr) << '\n'
               << "Prior support / resistance: " << (report.technical.supportResistance
                    ? price(report.technical.supportResistance->support) + " / " +
                      price(report.technical.supportResistance->resistance) : "N/A") << '\n'
               << "Market structure: ";
        if (report.technical.supportResistance && report.technical.latestClose) {
            if (*report.technical.latestClose > report.technical.supportResistance->resistance) {
                output << "latest close is above prior 20-candle resistance (upside break).\n";
            } else if (*report.technical.latestClose < report.technical.supportResistance->support) {
                output << "latest close is below prior 20-candle support (downside break).\n";
            } else {
                output << "latest close remains between prior 20-candle support and resistance.\n";
            }
        } else {
            output << "prior support/resistance structure unavailable.\n";
        }
        output << "Momentum: RSI 14 is "
               << (report.technical.rsi && *report.technical.rsi >= 55.0 ? "bullish" :
                   report.technical.rsi && *report.technical.rsi <= 45.0 ? "bearish" : "neutral")
               << "; MACD histogram is "
               << (report.technical.macd && report.technical.macd->histogram > 0.0
                    ? "positive" : report.technical.macd && report.technical.macd->histogram < 0.0
                        ? "negative" : "neutral") << ".\n"
               << "Trend and EMA assessment: " << trendName(report.technical) << ".\n";
        if (!report.scenario.conflicts.empty()) {
            output << "Indicator conflict: ";
            for (std::size_t index = 0; index < report.scenario.conflicts.size(); ++index) {
                if (index != 0) output << "; ";
                output << safeLine(report.scenario.conflicts[index], 180);
            }
            output << '\n';
        } else if (!report.scenario.reasons.empty()) {
            output << "Indicator assessment: " << safeLine(report.scenario.reasons.back(), 260) << '\n';
        }
    } else {
        output << report.status << ": " << safeLine(report.reason, 500) << '\n'
               << "Indicators: N/A — no stale or unsupported candles are used.\n";
    }

    output << "\nLatest News:\n";
    if (report.news.articles.empty()) {
        output << "No current verified gold-related headlines available.\n";
    } else {
        for (const auto& article : report.news.articles) {
            output << "• [" << newsImpactName(classifyGoldNews(article)) << "] "
                   << safeLine(article.headline, 230) << " — "
                   << safeLine(article.source, 100) << " | "
                   << localTimestamp(article.publishedAt, timezone) << '\n'
                   << safeLine(article.url, 700) << '\n';
        }
    }
    output << "\nGemini Assessment:\n"
           << (geminiAssessment.empty()
                ? (report.status == "COMPLETE"
                    ? "Gemini commentary unavailable; deterministic analysis is shown."
                    : "Not generated because validated completed M5 indicator history is unavailable.")
                : safeBlock(geminiAssessment, 1500)) << '\n'
           << "\nData Source: "
           << (report.technical.source.empty()
                ? safeLine(report.reason, 400) : safeLine(report.technical.source, 160)) << '\n'
           << "Last completed candle: "
           << (report.freshCompletedCandles
                ? localTimestamp(report.technical.latestCandleEnd, timezone) : "DATA UNAVAILABLE") << '\n'
           << "Analysis data status: " << report.status << '\n'
           << "No spot-price-derived candles or substitute timeframe are used.\n"
           << "All levels are hypothetical and for paper testing only; no real orders are placed.";
    auto message = output.str();
    return truncateUtf8(std::move(message), 3900);
}

std::string goldAnalysisChangeKey(const GoldMarketAnalysisReport& report,
                                  std::string_view importantNewsKey) {
    std::ostringstream material;
    material << report.status << '|' << trendName(report.technical) << '|'
             << stateName(report.scenario, report.status == "COMPLETE" &&
                 report.scenario.dataStatus == SignalDataStatus::Ready &&
                 report.scenario.state != SignalState::Wait &&
                 !validateSignalScenario(report.scenario).has_value()) << '|'
             << signalConfidenceName(report.scenario.confidence) << '|'
             << signalRiskLevelName(report.scenario.riskLevel) << '|';
    for (const auto& conflict : report.scenario.conflicts) material << conflict << ';';
    material << '|' << importantNewsKey;
    std::ostringstream key;
    key << "gold-m5-analysis:" << std::hex << fnv1a(material.str());
    return key.str();
}

} // namespace trading_bot
