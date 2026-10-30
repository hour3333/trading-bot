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

std::string formatUtc(Clock::time_point timestamp) {
    if (timestamp.time_since_epoch() <= Clock::duration::zero()) {
        return "DATA UNAVAILABLE";
    }
    const auto time = Clock::to_time_t(timestamp);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%d %H:%M:%S UTC");
    return output.str();
}

std::string formatPrice(double value) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2) << value;
    return output.str();
}

std::optional<Clock::duration> timeframeDuration(std::string_view timeframe) {
    using namespace std::chrono;
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

} // namespace

SignalScenario SignalEngine::evaluate(const MarketDataSnapshot& snapshot, Clock::time_point asOf) {
    SignalScenario scenario;
    scenario.market = snapshot.market;
    scenario.analysisAt = asOf;

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
    scenario.latestCandleClose = analysis.latestClose;
    scenario.sma20 = analysis.sma;
    scenario.emaFast = analysis.emaFast;
    scenario.emaSlow = analysis.emaSlow;
    scenario.rsi14 = analysis.rsi;
    if (analysis.macd) {
        scenario.macdHistogram = analysis.macd->histogram;
    }
    scenario.atr = analysis.atr;
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
    const double indicatorScale = std::max({1.0, std::abs(*analysis.latestClose),
                                            std::abs(analysis.macd->histogram)});
    const double macdTolerance = indicatorScale * 1e-12;
    const double priceTolerance = std::max(1.0, std::abs(*analysis.latestClose)) * 1e-12;

    std::array<DirectionalRule, 4> rules{{
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
        scenario.reasons.push_back(
            "The complete four-rule alignment is not present; no hypothetical levels were generated.");
        return scenario;
    }

    const double entry = quote.price;
    const double atr = *analysis.atr;
    const double halfWidth = atr * kEntryHalfWidthAtr;
    const double riskDistance = atr * (kEntryHalfWidthAtr + kStopDistanceBeyondEntryAtr);
    const double entryLow = entry - halfWidth;
    const double entryHigh = entry + halfWidth;
    const double stop = allBullish ? entry - riskDistance : entry + riskDistance;
    const double target1 = allBullish ? entry + riskDistance : entry - riskDistance;
    const double target2 = allBullish ? entry + 2.0 * riskDistance : entry - 2.0 * riskDistance;
    if (!std::isfinite(entryLow) || !std::isfinite(entryHigh) || !std::isfinite(stop) ||
        !std::isfinite(target1) || !std::isfinite(target2) || entryLow <= 0.0 ||
        stop <= 0.0 || target1 <= 0.0 || target2 <= 0.0 || riskDistance <= 0.0) {
        return waitFor(std::move(scenario), SignalDataStatus::InvalidData,
                       "Illustrative ATR levels could not be calculated as finite positive prices.");
    }

    scenario.state = allBullish ? SignalState::BuySetup : SignalState::SellSetup;
    scenario.entryZoneLow = entryLow;
    scenario.entryZoneHigh = entryHigh;
    scenario.stopLoss = stop;
    scenario.takeProfit1 = target1;
    scenario.takeProfit2 = target2;
    scenario.riskReward1 = 1.0;
    scenario.riskReward2 = 2.0;
    scenario.reasons.push_back(allBullish
        ? "All four deterministic rules align bullish; levels use the current quote and ATR."
        : "All four deterministic rules align bearish; levels use the current quote and ATR.");
    return scenario;
}

SignalScenario SignalEngine::evaluateHistorical(
    MarketSymbol market, std::span<const OhlcCandle> observedCandles) {
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
    return evaluate(snapshot, latest.endTime);
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
    case SignalDataStatus::InsufficientData:
        return "INSUFFICIENT DATA";
    case SignalDataStatus::InvalidData:
        return "DATA UNAVAILABLE";
    case SignalDataStatus::StaleData:
        return "STALE DATA";
    }
    return "DATA UNAVAILABLE";
}

std::string formatSignalScenario(const SignalScenario& scenario) {
    std::ostringstream output;
    output << "Educational paper-trading scenario only — not a recommendation.\n"
           << "Market: " << marketDisplayName(scenario.market) << " ("
           << marketSymbolName(scenario.market) << ")\n"
           << "Analysis date and time: " << formatUtc(scenario.analysisAt) << '\n'
           << "Timeframe: " << (scenario.timeframe.empty() ? "DATA UNAVAILABLE" : scenario.timeframe) << '\n'
           << "Trend: " << (scenario.dataStatus == SignalDataStatus::Ready
                                  ? trendClassificationName(scenario.trend)
                                  : signalDataStatusName(scenario.dataStatus)) << '\n'
           << "Signal state: " << signalStateName(scenario.state) << '\n'
           << "Data validation: " << signalDataStatusName(scenario.dataStatus) << '\n';

    if (scenario.state == SignalState::Wait || !scenario.entryZoneLow || !scenario.entryZoneHigh ||
        !scenario.stopLoss || !scenario.takeProfit1 || !scenario.takeProfit2 ||
        !scenario.riskReward1 || !scenario.riskReward2) {
        output << "Hypothetical entry zone: DATA UNAVAILABLE (WAIT)\n"
               << "Illustrative stop loss: DATA UNAVAILABLE (WAIT)\n"
               << "Illustrative TP1: DATA UNAVAILABLE (WAIT)\n"
               << "Illustrative TP2: DATA UNAVAILABLE (WAIT)\n"
               << "Risk/reward: DATA UNAVAILABLE (WAIT)\n";
    } else {
        output << "Hypothetical entry zone: $" << formatPrice(*scenario.entryZoneLow)
               << " – $" << formatPrice(*scenario.entryZoneHigh) << '\n'
               << "Illustrative stop loss: $" << formatPrice(*scenario.stopLoss) << '\n'
               << "Illustrative TP1: $" << formatPrice(*scenario.takeProfit1) << '\n'
               << "Illustrative TP2: $" << formatPrice(*scenario.takeProfit2) << '\n'
               << "Risk/reward: TP1 1:" << std::fixed << std::setprecision(2)
               << *scenario.riskReward1 << ", TP2 1:" << *scenario.riskReward2 << '\n';
    }

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
    output << "Data source: " << (scenario.dataSource.empty() ? "DATA UNAVAILABLE" : scenario.dataSource)
           << '\n'
           << "Quote timestamp: " << (scenario.quoteTimestamp
                                          ? formatUtc(*scenario.quoteTimestamp)
                                          : "DATA UNAVAILABLE") << '\n'
           << "Latest candle timestamp: " << (scenario.candleTimestamp
                                                  ? formatUtc(*scenario.candleTimestamp)
                                                  : "DATA UNAVAILABLE");
    return output.str();
}

} // namespace trading_bot
