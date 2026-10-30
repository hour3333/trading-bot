#include "trading_bot/signal_engine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace trading_bot {
namespace {

using Clock = std::chrono::system_clock;

constexpr auto kMaximumQuoteAge = std::chrono::minutes(10);
constexpr auto kMaximumFetchAge = std::chrono::minutes(15);
constexpr auto kAllowedFutureSkew = std::chrono::minutes(5);
constexpr double kEntryHalfWidthAtr = 0.10;
constexpr double kStopDistanceBeyondEntryAtr = 1.0;

std::string formatLocalTimestamp(Clock::time_point timestamp, std::string_view timezone) {
    if (timestamp.time_since_epoch() <= Clock::duration::zero()) {
        return "DATA UNAVAILABLE";
    }
    const auto time = Clock::to_time_t(timestamp);
    std::tm local{};
    if (localtime_r(&time, &local) == nullptr) return "DATA UNAVAILABLE";
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << " (" << timezone << ')';
    return output.str();
}

std::string formatPrice(double value) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2) << value;
    return output.str();
}

std::optional<Clock::duration> timeframeDuration(std::string_view timeframe) {
    using namespace std::chrono;
    if (timeframe == "5m") {
        return minutes(5);
    }
    if (timeframe == "15m") {
        return minutes(15);
    }
    if (timeframe == "1h") {
        return hours(1);
    }
    if (timeframe == "30m") {
        return minutes(30);
    }
    if (timeframe == "4h") {
        return hours(4);
    }
    if (timeframe == "4d") {
        return days(4);
    }
    return std::nullopt;
}

void clearLevels(SignalScenario& scenario) {
    scenario.entryZoneLow.reset();
    scenario.entryZoneHigh.reset();
    scenario.stopLoss.reset();
    scenario.takeProfit1.reset();
    scenario.takeProfit2.reset();
    scenario.riskReward1.reset();
    scenario.riskReward2.reset();
}

SignalScenario waitFor(SignalScenario scenario, SignalDataStatus status,
                       std::string reason) {
    scenario.state = SignalState::Wait;
    scenario.dataStatus = status;
    clearLevels(scenario);
    scenario.reasons.push_back(std::move(reason));
    scenario.confidence = SignalConfidence::Unconfirmed;
    scenario.confidenceReason = "Required timeframe, data freshness, or candle history is unconfirmed.";
    scenario.riskLevel = SignalRiskLevel::High;
    scenario.riskReason = "Risk is high/unassessable because the validated scenario is unavailable.";
    return scenario;
}

bool validTimestamp(Clock::time_point timestamp) {
    return timestamp.time_since_epoch() > Clock::duration::zero();
}

bool isRecent(Clock::time_point timestamp, Clock::time_point asOf,
              Clock::duration maximumAge) {
    return validTimestamp(timestamp) && timestamp <= asOf + kAllowedFutureSkew &&
           timestamp >= asOf - maximumAge;
}

struct DirectionalRule {
    std::string name;
    int direction{0}; // +1 bullish, -1 bearish, 0 neutral.
};

std::string directionName(int direction) {
    if (direction > 0) {
        return "bullish";
    }
    if (direction < 0) {
        return "bearish";
    }
    return "neutral";
}

struct RequestedSignalTimeframe {
    std::string display;
    std::string interval;
};

std::optional<RequestedSignalTimeframe> requestedSignalTimeframe(std::string_view value) {
    if (value == "M5" || value == "m5" || value == "5m") return RequestedSignalTimeframe{"M5", "5m"};
    if (value == "M15" || value == "m15" || value == "15m") return RequestedSignalTimeframe{"M15", "15m"};
    if (value == "H1" || value == "h1" || value == "1h") return RequestedSignalTimeframe{"H1", "1h"};
    if (value == "H4" || value == "h4" || value == "4h") return RequestedSignalTimeframe{"H4", "4h"};
    return std::nullopt;
}

std::string maybePrice(const std::optional<double>& value) {
    return value && std::isfinite(*value) && *value > 0.0 ? "$" + formatPrice(*value)
                                                         : "DATA UNAVAILABLE";
}

} // namespace

namespace {

SignalScenario evaluateSnapshot(const MarketDataSnapshot& snapshot, Clock::time_point asOf,
                                double minimumRiskReward = 1.5) {
    SignalScenario scenario;
    scenario.market = snapshot.market;
    scenario.analysisAt = asOf;
    scenario.minimumRiskReward = minimumRiskReward;

    if (snapshot.quote) {
        scenario.quotePrice = snapshot.quote->price;
        scenario.quoteTimestamp = snapshot.quote->providerTimestamp;
        scenario.dataSource = snapshot.quote->source;
    }

    const auto analysis = TechnicalAnalysis::analyze(snapshot.market, snapshot.candles, asOf);
    scenario.timeframe = analysis.timeframe;
    scenario.trend = analysis.trend;
    if (validTimestamp(analysis.latestCandleEnd)) {
        scenario.candleTimestamp = analysis.latestCandleEnd;
    }
    if (!analysis.source.empty()) {
        if (scenario.dataSource.empty()) {
            scenario.dataSource = analysis.source;
        } else if (scenario.dataSource != analysis.source) {
            scenario.dataSource += " (candles: " + analysis.source + ")";
        }
    }

    if (analysis.status == AnalysisStatus::InvalidData) {
        return waitFor(std::move(scenario), SignalDataStatus::InvalidData,
                       "Invalid candle history: " + analysis.reason);
    }
    if (analysis.status != AnalysisStatus::Complete) {
        return waitFor(std::move(scenario), SignalDataStatus::InsufficientData,
                       "Technical analysis unavailable: " + analysis.reason);
    }
    if (!std::isfinite(minimumRiskReward) || minimumRiskReward < 0.5 ||
        minimumRiskReward > 10.0) {
        return waitFor(std::move(scenario), SignalDataStatus::InvalidData,
                       "Configured minimum risk/reward must be between 0.5 and 10.");
    }
    scenario.latestCandleClose = analysis.latestClose;
    scenario.sma20 = analysis.sma;
    scenario.ema20 = analysis.ema20;
    scenario.ema50 = analysis.ema50;
    scenario.emaFast = analysis.emaFast;
    scenario.emaSlow = analysis.emaSlow;
    scenario.rsi14 = analysis.rsi;
    if (analysis.macd) {
        scenario.macdHistogram = analysis.macd->histogram;
    }
    scenario.atr = analysis.atr;
    if (analysis.supportResistance) {
        scenario.support = analysis.supportResistance->support;
        scenario.resistance = analysis.supportResistance->resistance;
    }
    if (!snapshot.quote) {
        return waitFor(std::move(scenario), SignalDataStatus::InsufficientData,
                       "A current provider quote is required; no quote was returned.");
    }

    const auto& quote = *snapshot.quote;
    const std::string expectedSymbol = marketSymbolName(snapshot.market);
    if (quote.symbol != expectedSymbol || quote.source.empty() || !std::isfinite(quote.price) ||
        quote.price <= 0.0 || !validTimestamp(quote.providerTimestamp) ||
        !validTimestamp(quote.fetchedAt)) {
        scenario.quotePrice.reset();
        if (!validTimestamp(quote.providerTimestamp)) {
            scenario.quoteTimestamp.reset();
        }
        return waitFor(std::move(scenario), SignalDataStatus::InvalidData,
                       "Provider quote is missing required market, source, price, or timestamp data.");
    }
    if (quote.providerTimestamp > asOf + kAllowedFutureSkew || quote.fetchedAt > asOf + kAllowedFutureSkew) {
        if (quote.providerTimestamp > asOf + kAllowedFutureSkew) {
            scenario.quoteTimestamp.reset();
        }
        scenario.quotePrice.reset();
        return waitFor(std::move(scenario), SignalDataStatus::InvalidData,
                       "Provider quote contains a future timestamp.");
    }
    if (quote.delayed || !isRecent(quote.providerTimestamp, asOf, kMaximumQuoteAge) ||
        !isRecent(quote.fetchedAt, asOf, kMaximumFetchAge)) {
        return waitFor(std::move(scenario), SignalDataStatus::StaleData,
                       "Provider quote is delayed or outside the freshness window.");
    }
    if (analysis.delayed || snapshot.candles.empty() ||
        !isRecent(snapshot.candles.back().fetchedAt, asOf, kMaximumFetchAge)) {
        return waitFor(std::move(scenario), SignalDataStatus::StaleData,
                       "The latest OHLC data is delayed or outside the freshness window.");
    }

    const auto duration = timeframeDuration(analysis.timeframe);
    if (!duration) {
        return waitFor(std::move(scenario), SignalDataStatus::InsufficientData,
                       "The provider candle interval is unsupported for educational scenarios.");
    }
    if (analysis.latestCandleEnd > asOf || asOf - analysis.latestCandleEnd > *duration * 2) {
        return waitFor(std::move(scenario), SignalDataStatus::StaleData,
                       "The latest completed candle is outside the timeframe freshness window.");
    }
    if (!analysis.latestClose || !analysis.sma || !analysis.rsi || !analysis.macd ||
        !analysis.atr || !std::isfinite(*analysis.latestClose) ||
        !std::isfinite(*analysis.sma) || !std::isfinite(*analysis.rsi) ||
        !std::isfinite(analysis.macd->histogram) || !std::isfinite(*analysis.atr) ||
        *analysis.latestClose <= 0.0 || *analysis.sma <= 0.0 || *analysis.atr <= 0.0) {
        return waitFor(std::move(scenario), SignalDataStatus::InvalidData,
                       "One or more required technical indicator values are invalid.");
    }
    const double maximumQuoteDeviation = std::max(*analysis.atr * 3.0,
                                                   *analysis.latestClose * 0.02);
    if (std::abs(quote.price - *analysis.latestClose) > maximumQuoteDeviation) {
        return waitFor(std::move(scenario), SignalDataStatus::InvalidData,
                       "The current quote is inconsistent with the latest candle close.");
    }

    scenario.dataStatus = SignalDataStatus::Ready;
    const double hypotheticalStopDistance = *analysis.atr *
        (kEntryHalfWidthAtr + kStopDistanceBeyondEntryAtr);
    const double volatilityRatio = std::max(*analysis.atr, hypotheticalStopDistance) /
                                   *analysis.latestClose;
    if (volatilityRatio >= 0.02) {
        scenario.riskLevel = SignalRiskLevel::High;
        scenario.riskReason = "ATR/stop distance is at least 2% of close under the deterministic volatility rule.";
    } else if (volatilityRatio >= 0.01) {
        scenario.riskLevel = SignalRiskLevel::Medium;
        scenario.riskReason = "ATR/stop distance is between 1% and 2% of close under the deterministic volatility rule.";
    } else {
        scenario.riskLevel = SignalRiskLevel::Low;
        scenario.riskReason = "ATR/stop distance is below 1% of close under the deterministic volatility rule.";
    }
    const double indicatorScale = std::max({1.0, std::abs(*analysis.latestClose),
                                            std::abs(analysis.macd->histogram)});
    const double macdTolerance = indicatorScale * 1e-12;
    const double priceTolerance = std::max(1.0, std::abs(*analysis.latestClose)) * 1e-12;

    const double structureTolerance = std::max(1.0, std::abs(*analysis.latestClose)) * 1e-12;
    const int structureDirection = analysis.supportResistance &&
        *analysis.latestClose > analysis.supportResistance->resistance + structureTolerance
            ? 1
            : analysis.supportResistance &&
              *analysis.latestClose < analysis.supportResistance->support - structureTolerance
                ? -1 : 0;
    std::array<DirectionalRule, 5> rules{{
        {"Trend", analysis.trend == TrendClassification::Bullish
                      ? 1
                      : analysis.trend == TrendClassification::Bearish ? -1 : 0},
        {"MACD histogram", analysis.macd->histogram > macdTolerance
                               ? 1
                               : analysis.macd->histogram < -macdTolerance ? -1 : 0},
        {"RSI(14)", *analysis.rsi >= 55.0 ? 1 : *analysis.rsi <= 45.0 ? -1 : 0},
        {"Close vs SMA(20)", *analysis.latestClose > *analysis.sma + priceTolerance
                                  ? 1
                                  : *analysis.latestClose < *analysis.sma - priceTolerance ? -1 : 0},
        {"Price structure vs prior 20-candle range", structureDirection},
    }};

    for (const auto& rule : rules) {
        scenario.reasons.push_back(rule.name + " is " + directionName(rule.direction) + ".");
    }
    for (std::size_t left = 0; left < rules.size(); ++left) {
        for (std::size_t right = left + 1; right < rules.size(); ++right) {
            if (rules[left].direction != 0 && rules[right].direction != 0 &&
                rules[left].direction != rules[right].direction) {
                scenario.conflicts.push_back(rules[left].name + " is " +
                    directionName(rules[left].direction) + " while " + rules[right].name +
                    " is " + directionName(rules[right].direction) + ".");
            }
        }
    }
    if (!scenario.conflicts.empty()) {
        scenario.confidence = SignalConfidence::Low;
        scenario.confidenceReason = "Conflicting indicators disagree; trend, momentum, moving-average, and price-structure alignment is not established.";
        scenario.reasons.push_back("Conflicting indicators detected; state remains WAIT.");
        return scenario;
    }

    const bool allBullish = std::all_of(rules.begin(), rules.end(), [](const auto& rule) {
        return rule.direction > 0;
    });
    const bool allBearish = std::all_of(rules.begin(), rules.end(), [](const auto& rule) {
        return rule.direction < 0;
    });
    if (!allBullish && !allBearish) {
        const auto bullishVotes = std::count_if(rules.begin(), rules.end(), [](const auto& rule) {
            return rule.direction > 0;
        });
        const auto bearishVotes = std::count_if(rules.begin(), rules.end(), [](const auto& rule) {
            return rule.direction < 0;
        });
        if (std::max(bullishVotes, bearishVotes) >= 3) {
            scenario.confidence = SignalConfidence::Medium;
            scenario.confidenceReason = "Three or four of five deterministic checks align, but the complete setup rule is not met.";
        } else {
            scenario.confidence = SignalConfidence::Low;
            scenario.confidenceReason = "Fewer than three deterministic checks align in one direction.";
        }
        scenario.reasons.push_back(
            "The complete five-rule alignment is not present; no hypothetical levels were generated.");
        return scenario;
    }

    const double entry = quote.price;
    const double atr = *analysis.atr;
    const double halfWidth = atr * kEntryHalfWidthAtr;
    const double riskDistance = atr * (kEntryHalfWidthAtr + kStopDistanceBeyondEntryAtr);
    const double entryLow = entry - halfWidth;
    const double entryHigh = entry + halfWidth;
    const double stop = allBullish ? entry - riskDistance : entry + riskDistance;
    const double target1 = allBullish ? entry + minimumRiskReward * riskDistance
                                      : entry - minimumRiskReward * riskDistance;
    const double target2Ratio = std::max(2.0, minimumRiskReward + 0.5);
    const double target2 = allBullish ? entry + target2Ratio * riskDistance
                                      : entry - target2Ratio * riskDistance;
    if (!std::isfinite(entryLow) || !std::isfinite(entryHigh) || !std::isfinite(stop) ||
        !std::isfinite(target1) || !std::isfinite(target2) || entryLow <= 0.0 ||
        stop <= 0.0 || target1 <= 0.0 || target2 <= 0.0 || riskDistance <= 0.0) {
        return waitFor(std::move(scenario), SignalDataStatus::InvalidData,
                       "Illustrative ATR levels could not be calculated as finite positive prices.");
    }

    scenario.state = allBullish ? SignalState::BuySetup : SignalState::SellSetup;
    scenario.minimumRiskReward = minimumRiskReward;
    scenario.entryZoneLow = entryLow;
    scenario.entryZoneHigh = entryHigh;
    scenario.stopLoss = stop;
    scenario.takeProfit1 = target1;
    scenario.takeProfit2 = target2;
    scenario.riskReward1 = minimumRiskReward;
    scenario.riskReward2 = target2Ratio;
    scenario.confidence = SignalConfidence::High;
    scenario.confidenceReason = "EMA20/EMA50 trend, MACD histogram, RSI(14), close versus SMA(20), and prior-range price structure all align; categorical assessment, not a statistical probability.";
    scenario.reasons.push_back(allBullish
        ? "All five deterministic rules align bullish; entry and invalidation use the provider quote and ATR."
        : "All five deterministic rules align bearish; entry and invalidation use the provider quote and ATR.");
    scenario.reasons.push_back("TP1 meets the configured minimum risk/reward of 1:" +
                               formatPrice(minimumRiskReward) + ".");
    if (const auto validation = validateSignalScenario(scenario)) {
        return waitFor(std::move(scenario), SignalDataStatus::InvalidData,
                       "Signal validation rejected the calculated levels: " + *validation);
    }
    return scenario;
}

} // namespace

SignalScenario SignalEngine::evaluate(const MarketDataSnapshot& snapshot, Clock::time_point asOf) {
    if (snapshot.candles.empty()) {
        return evaluateSnapshot(snapshot, asOf);
    }
    const auto requested = requestedSignalTimeframe(snapshot.candles.front().interval);
    if (!requested || std::any_of(snapshot.candles.begin(), snapshot.candles.end(),
            [&](const OhlcCandle& candle) { return candle.interval != requested->interval; })) {
        SignalScenario scenario;
        scenario.market = snapshot.market;
        scenario.analysisAt = asOf;
        scenario.timeframe = snapshot.candles.front().interval;
        scenario.dataSource = snapshot.candles.front().source;
        scenario.candleTimestamp = snapshot.candles.back().endTime;
        if (snapshot.quote) {
            scenario.quotePrice = snapshot.quote->price;
            scenario.quoteTimestamp = snapshot.quote->providerTimestamp;
        }
        return waitFor(std::move(scenario), SignalDataStatus::InsufficientData,
                       "Live signals support M5, M15, H1, and H4 only; provider interval " +
                           snapshot.candles.front().interval + " is not eligible.");
    }
    return evaluateSnapshot(snapshot, asOf);
}

SignalScenario SignalEngine::evaluate(const MarketDataSnapshot& snapshot,
                                     std::string_view timeframe, Clock::time_point asOf,
                                     double minimumRiskReward) {
    const auto requested = requestedSignalTimeframe(timeframe);
    if (!requested) {
        SignalScenario scenario;
        scenario.market = snapshot.market;
        scenario.analysisAt = asOf;
        scenario.timeframe = std::string(timeframe);
        return waitFor(std::move(scenario), SignalDataStatus::InsufficientData,
                       "Unsupported timeframe; use M5, M15, H1, or H4.");
    }

    if (snapshot.candles.empty() || std::any_of(snapshot.candles.begin(), snapshot.candles.end(),
            [&](const OhlcCandle& candle) { return candle.interval != requested->interval; })) {
        SignalScenario scenario;
        scenario.market = snapshot.market;
        scenario.analysisAt = asOf;
        scenario.timeframe = requested->display;
        if (snapshot.quote) {
            scenario.quotePrice = snapshot.quote->price;
            scenario.quoteTimestamp = snapshot.quote->providerTimestamp;
            scenario.dataSource = snapshot.quote->source;
        }
        const std::string availability = snapshot.candles.empty()
            ? (snapshot.historyDescription.empty()
                ? "No genuine " + requested->display + " OHLC candles are available from the configured provider."
                : snapshot.historyDescription + ". No genuine " + requested->display + " OHLC candles are available.")
            : "Provider candles do not match the requested " + requested->display + " timeframe.";
        return waitFor(std::move(scenario), snapshot.candles.empty()
                           ? SignalDataStatus::DataUnavailable
                           : SignalDataStatus::InsufficientData, availability);
    }

    auto scenario = evaluateSnapshot(snapshot, asOf, minimumRiskReward);
    scenario.timeframe = requested->display;
    if (scenario.state != SignalState::Wait) {
        if (const auto validation = validateSignalScenario(scenario)) {
            return waitFor(std::move(scenario), SignalDataStatus::InvalidData,
                           "Signal validation rejected the scenario: " + *validation);
        }
    }
    return scenario;
}

SignalScenario SignalEngine::evaluateHistorical(
    MarketSymbol market, std::span<const OhlcCandle> observedCandles,
    double minimumRiskReward) {
    if (observedCandles.empty()) {
        MarketDataSnapshot empty;
        empty.market = market;
        return evaluate(empty, Clock::time_point{});
    }

    // Backtests know the completed bar only at its close. Normalize fetch/delay
    // metadata on this private copy to that simulated observation time; candle
    // prices, timestamps, symbol, source, and interval remain untouched.
    std::vector<OhlcCandle> replayCandles(observedCandles.begin(), observedCandles.end());
    for (auto& candle : replayCandles) {
        candle.fetchedAt = candle.endTime;
        candle.delayed = false;
    }
    const auto latest = replayCandles.back();
    MarketDataSnapshot snapshot;
    snapshot.market = market;
    snapshot.historyDescription = "Historical OHLC replay";
    snapshot.candles = std::move(replayCandles);
    snapshot.quote = MarketQuote{
        marketSymbolName(market), latest.source, latest.close,
        latest.endTime, latest.endTime, false};
    return evaluateSnapshot(snapshot, latest.endTime, minimumRiskReward);
}

std::string signalStateName(SignalState state) {
    switch (state) {
    case SignalState::BuySetup:
        return "BUY SETUP";
    case SignalState::SellSetup:
        return "SELL SETUP";
    case SignalState::Wait:
        return "WAIT";
    }
    return "WAIT";
}

std::string signalDataStatusName(SignalDataStatus status) {
    switch (status) {
    case SignalDataStatus::Ready:
        return "READY";
    case SignalDataStatus::DataUnavailable:
        return "DATA UNAVAILABLE";
    case SignalDataStatus::InsufficientData:
        return "INSUFFICIENT DATA";
    case SignalDataStatus::InvalidData:
        return "DATA UNAVAILABLE";
    case SignalDataStatus::StaleData:
        return "STALE DATA";
    }
    return "DATA UNAVAILABLE";
}

std::string signalConfidenceName(SignalConfidence confidence) {
    switch (confidence) {
    case SignalConfidence::High: return "High";
    case SignalConfidence::Medium: return "Medium";
    case SignalConfidence::Low: return "Low";
    case SignalConfidence::Unconfirmed: return "Unconfirmed";
    }
    return "Unconfirmed";
}

std::string signalRiskLevelName(SignalRiskLevel risk) {
    switch (risk) {
    case SignalRiskLevel::Low: return "Low";
    case SignalRiskLevel::Medium: return "Medium";
    case SignalRiskLevel::High: return "High";
    }
    return "High";
}

std::optional<std::string> validateSignalScenario(const SignalScenario& scenario) {
    const bool hasAnyLevel = scenario.entryZoneLow || scenario.entryZoneHigh || scenario.stopLoss ||
                             scenario.takeProfit1 || scenario.takeProfit2 || scenario.riskReward1 ||
                             scenario.riskReward2;
    if (scenario.state == SignalState::Wait) {
        if (hasAnyLevel) return "WAIT scenarios must not contain illustrative levels.";
        return std::nullopt;
    }
    if (scenario.dataStatus != SignalDataStatus::Ready) {
        return "actionable-looking states require READY validated data.";
    }
    if (!scenario.quotePrice || !scenario.latestCandleClose || !scenario.atr ||
        !scenario.sma20 || !scenario.ema20 || !scenario.ema50 || !scenario.rsi14 ||
        !scenario.macdHistogram || !scenario.support || !scenario.resistance ||
        !scenario.entryZoneLow || !scenario.entryZoneHigh || !scenario.stopLoss ||
        !scenario.takeProfit1 || !scenario.takeProfit2 || !scenario.riskReward1 ||
        !scenario.riskReward2) {
        return "one or more required prices, indicators, or ratios are missing.";
    }
    const std::array<double, 17> values{
        *scenario.quotePrice, *scenario.latestCandleClose, *scenario.atr, *scenario.sma20,
        *scenario.ema20, *scenario.ema50, *scenario.rsi14, *scenario.macdHistogram,
        *scenario.support, *scenario.resistance, *scenario.entryZoneLow, *scenario.entryZoneHigh,
        *scenario.stopLoss, *scenario.takeProfit1, *scenario.takeProfit2,
        *scenario.riskReward1, *scenario.riskReward2};
    if (!std::all_of(values.begin(), values.end(), [](double value) {
            return std::isfinite(value);
        })) {
        return "prices, indicators, and risk/reward ratios must be finite.";
    }
    if (*scenario.quotePrice <= 0.0 || *scenario.latestCandleClose <= 0.0 ||
        *scenario.atr <= 0.0 || *scenario.sma20 <= 0.0 || *scenario.ema20 <= 0.0 ||
        *scenario.ema50 <= 0.0 || *scenario.rsi14 < 0.0 || *scenario.rsi14 > 100.0 ||
        *scenario.support <= 0.0 || *scenario.resistance <= 0.0 ||
        *scenario.entryZoneLow <= 0.0 || *scenario.entryZoneHigh <= 0.0 ||
        *scenario.stopLoss <= 0.0 || *scenario.takeProfit1 <= 0.0 ||
        *scenario.takeProfit2 <= 0.0 ||
        *scenario.support > *scenario.resistance || *scenario.riskReward1 <= 0.0 ||
        *scenario.riskReward2 <= 0.0 || scenario.confidence == SignalConfidence::Unconfirmed ||
        !std::isfinite(scenario.minimumRiskReward) || scenario.minimumRiskReward < 0.5 ||
        scenario.minimumRiskReward > 10.0 || scenario.confidenceReason.empty() ||
        scenario.riskReason.empty()) {
        return "prices, support/resistance, RSI, or risk/reward values are outside valid ranges.";
    }
    if (*scenario.entryZoneLow >= *scenario.entryZoneHigh) {
        return "entry zone lower bound must be below its upper bound.";
    }
    if (scenario.state == SignalState::BuySetup) {
        if (scenario.trend != TrendClassification::Bullish ||
            *scenario.ema20 <= *scenario.ema50 ||
            *scenario.latestCandleClose <= *scenario.ema20 ||
            *scenario.latestCandleClose <= *scenario.sma20 ||
            *scenario.latestCandleClose <= *scenario.resistance ||
            *scenario.rsi14 < 55.0 || *scenario.macdHistogram <= 0.0 ||
            !(*scenario.stopLoss < *scenario.entryZoneLow &&
              *scenario.entryZoneHigh < *scenario.takeProfit1 &&
              *scenario.takeProfit1 < *scenario.takeProfit2)) {
            return "BUY levels or trend are not in logical order.";
        }
    } else if (scenario.trend != TrendClassification::Bearish ||
               *scenario.ema20 >= *scenario.ema50 ||
               *scenario.latestCandleClose >= *scenario.ema20 ||
               *scenario.latestCandleClose >= *scenario.sma20 ||
               *scenario.latestCandleClose >= *scenario.support ||
               *scenario.rsi14 > 45.0 || *scenario.macdHistogram >= 0.0 ||
               !(*scenario.stopLoss > *scenario.entryZoneHigh &&
                 *scenario.entryZoneLow > *scenario.takeProfit1 &&
                 *scenario.takeProfit1 > *scenario.takeProfit2)) {
        return "SELL levels or trend are not in logical order.";
    }
    if (!scenario.conflicts.empty()) {
        return "directional scenario contains conflicting indicators.";
    }
    const double midpoint = (*scenario.entryZoneLow + *scenario.entryZoneHigh) / 2.0;
    const double risk = std::abs(midpoint - *scenario.stopLoss);
    if (!std::isfinite(risk) || risk <= 0.0) {
        return "entry-to-stop risk distance is invalid.";
    }
    const double expectedRr1 = std::abs(*scenario.takeProfit1 - midpoint) / risk;
    const double expectedRr2 = std::abs(*scenario.takeProfit2 - midpoint) / risk;
    const auto ratioMatches = [](double actual, double expected) {
        return std::isfinite(actual) && actual > 0.0 &&
               std::abs(actual - expected) <= std::max(1e-8, std::abs(expected) * 1e-6);
    };
    if (!ratioMatches(*scenario.riskReward1, expectedRr1) ||
        !ratioMatches(*scenario.riskReward2, expectedRr2)) {
        return "risk/reward ratios do not match the validated entry, stop, and targets.";
    }
    if (*scenario.riskReward1 + 1e-9 < scenario.minimumRiskReward) {
        return "TP1 does not meet the configured minimum risk/reward ratio.";
    }
    return std::nullopt;
}

std::string formatSignalScenario(const SignalScenario& scenario, std::string_view timezone) {
    std::ostringstream output;
    std::string timeframe = scenario.timeframe;
    if (timeframe == "5m") timeframe = "M5";
    else if (timeframe == "15m") timeframe = "M15";
    else if (timeframe == "1h") timeframe = "H1";
    else if (timeframe == "4h") timeframe = "H4";
    const auto marketName = scenario.market == MarketSymbol::XauUsd ? "GOLD" : "BITCOIN";
    const auto analysisTimestamp = formatLocalTimestamp(scenario.analysisAt, timezone);
    output << marketSymbolName(scenario.market) << " — " << marketName << '\n'
           << "Educational paper-trading scenario only; no real orders are placed.\n"
           << "Date: " << (analysisTimestamp == "DATA UNAVAILABLE"
                ? analysisTimestamp : analysisTimestamp.substr(0, 10)) << '\n'
           << "Time: " << (analysisTimestamp == "DATA UNAVAILABLE"
                ? analysisTimestamp : analysisTimestamp.substr(11, 5) + " (" + std::string(timezone) + ")") << '\n'
           << "Timeframe: " << (timeframe.empty() ? "DATA UNAVAILABLE" : timeframe) << '\n'
           << "Data source: " << (scenario.dataSource.empty() ? "DATA UNAVAILABLE" : scenario.dataSource) << '\n'
           << "Candle timestamp: " << (scenario.candleTimestamp
                  ? formatLocalTimestamp(*scenario.candleTimestamp, timezone) : "DATA UNAVAILABLE") << '\n'
           << "Trend: " << (scenario.dataStatus == SignalDataStatus::Ready
                  ? trendClassificationName(scenario.trend) : "UNCONFIRMED — insufficient validated candles") << '\n'
           << "Signal: " << signalStateName(scenario.state) << " — PAPER ONLY\n"
           << "Data validation: " << signalDataStatusName(scenario.dataStatus) << '\n'
           << "Data freshness: "
           << (scenario.dataStatus == SignalDataStatus::Ready
                ? "FRESH — quote and completed candle passed age checks"
                : scenario.dataStatus == SignalDataStatus::StaleData
                    ? "STALE — setup suppressed"
                    : "UNCONFIRMED — setup suppressed") << "\n\n"
           << "Parameter | Price / Assessment\n"
           << "--------------------------------\n";

    if (scenario.state == SignalState::Wait || !scenario.entryZoneLow || !scenario.entryZoneHigh ||
        !scenario.stopLoss || !scenario.takeProfit1 || !scenario.takeProfit2 ||
        !scenario.riskReward1 || !scenario.riskReward2) {
        output << "Entry zone to watch | DATA UNAVAILABLE — WAIT\n"
               << "Stop loss | DATA UNAVAILABLE — WAIT\n"
               << "Take Profit 1 | DATA UNAVAILABLE — WAIT\n"
               << "Take Profit 2 | DATA UNAVAILABLE — WAIT\n"
               << "Risk/reward | DATA UNAVAILABLE — WAIT\n";
    } else {
        output << "Entry zone to watch | $" << formatPrice(*scenario.entryZoneLow)
               << "–$" << formatPrice(*scenario.entryZoneHigh) << '\n'
               << "Stop loss | $" << formatPrice(*scenario.stopLoss) << '\n'
               << "Take Profit 1 | $" << formatPrice(*scenario.takeProfit1) << '\n'
               << "Take Profit 2 | $" << formatPrice(*scenario.takeProfit2) << '\n'
               << "Risk/reward | TP1 1:" << std::fixed << std::setprecision(2)
               << *scenario.riskReward1 << ", TP2 1:" << *scenario.riskReward2 << '\n';
    }
    output << "Reference price | " << (scenario.dataStatus == SignalDataStatus::Ready
        ? maybePrice(scenario.quotePrice) : "DATA UNAVAILABLE — WAIT") << '\n';

    output << "Confidence | " << signalConfidenceName(scenario.confidence)
           << " — " << scenario.confidenceReason << '\n'
           << "Risk | " << signalRiskLevelName(scenario.riskLevel)
           << " — " << scenario.riskReason << '\n'
           << "Indicators | RSI(14): " << (scenario.rsi14 ? formatPrice(*scenario.rsi14) : "DATA UNAVAILABLE")
           << "; MACD histogram: " << (scenario.macdHistogram ? formatPrice(*scenario.macdHistogram) : "DATA UNAVAILABLE")
           << "; EMA20: " << maybePrice(scenario.ema20)
           << "; EMA50: " << maybePrice(scenario.ema50)
           << "; SMA20: " << maybePrice(scenario.sma20)
           << "; ATR: " << maybePrice(scenario.atr)
           << "; support/resistance: " << maybePrice(scenario.support)
           << " / " << maybePrice(scenario.resistance) << '\n';

    output << "Reasons:\n";
    if (scenario.reasons.empty()) {
        output << "- No scenario generated.\n";
    } else {
        for (const auto& reason : scenario.reasons) {
            output << "- " << reason << '\n';
        }
    }
    if (!scenario.conflicts.empty()) {
        output << "Conflicts:\n";
        for (const auto& conflict : scenario.conflicts) {
            output << "- " << conflict << '\n';
        }
    }
    output << "Quote timestamp: " << (scenario.quoteTimestamp
               ? formatLocalTimestamp(*scenario.quoteTimestamp, timezone) : "DATA UNAVAILABLE") << '\n'
           << "Source timestamps are provider-reported where available. Categorical confidence is rule-based, not a measured probability.";
    return output.str();
}

} // namespace trading_bot
