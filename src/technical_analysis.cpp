#include "trading_bot/technical_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace trading_bot {
namespace {

constexpr std::size_t kMaximumPeriod = 1'000'000;

bool allFinite(std::span<const double> values) {
    return std::all_of(values.begin(), values.end(), [](double value) {
        return std::isfinite(value);
    });
}

std::optional<double> average(std::span<const double> values) {
    if (values.empty() || !allFinite(values)) {
        return std::nullopt;
    }
    // Divide before summing to avoid overflowing when otherwise valid values
    // have a large magnitude.
    long double result = 0.0L;
    const auto divisor = static_cast<long double>(values.size());
    for (const auto value : values) {
        result += static_cast<long double>(value) / divisor;
    }
    const auto converted = static_cast<double>(result);
    return std::isfinite(converted) ? std::optional<double>(converted) : std::nullopt;
}

std::vector<double> emaSeries(std::span<const double> values, std::size_t period) {
    if (period == 0 || period > kMaximumPeriod || values.size() < period || !allFinite(values)) {
        return {};
    }
    std::vector<double> result(values.size(), std::numeric_limits<double>::quiet_NaN());
    const auto seed = average(values.first(period));
    if (!seed) {
        return {};
    }
    result[period - 1] = *seed;
    const double alpha = 2.0 / (static_cast<double>(period) + 1.0);
    for (std::size_t index = period; index < values.size(); ++index) {
        result[index] = alpha * values[index] + (1.0 - alpha) * result[index - 1];
        if (!std::isfinite(result[index])) {
            return {};
        }
    }
    return result;
}

std::optional<MarketTimeframe> parseTimeframe(std::string_view interval) {
    if (interval == "5m") {
        return MarketTimeframe::M5;
    }
    if (interval == "15m") {
        return MarketTimeframe::M15;
    }
    if (interval == "1h") {
        return MarketTimeframe::H1;
    }
    if (interval == "30m" || interval == "30-minute provider candle") {
        return MarketTimeframe::M30;
    }
    if (interval == "4h" || interval == "4-hour provider candle") {
        return MarketTimeframe::H4;
    }
    if (interval == "4d" || interval == "4-day provider candle") {
        return MarketTimeframe::D4;
    }
    return std::nullopt;
}

std::chrono::system_clock::duration timeframeDuration(MarketTimeframe timeframe) {
    using namespace std::chrono;
    switch (timeframe) {
    case MarketTimeframe::M5:
        return minutes(5);
    case MarketTimeframe::M15:
        return minutes(15);
    case MarketTimeframe::H1:
        return hours(1);
    case MarketTimeframe::M30:
        return minutes(30);
    case MarketTimeframe::H4:
        return hours(4);
    case MarketTimeframe::D4:
        return days(4);
    }
    return system_clock::duration::zero();
}

std::optional<std::size_t> minimumCandles(const TechnicalAnalysisSettings& settings) {
    const auto validPeriod = [](std::size_t period) {
        return period > 0 && period <= kMaximumPeriod;
    };
    if (!validPeriod(settings.smaPeriod) || !validPeriod(settings.trendFastEmaPeriod) ||
        !validPeriod(settings.trendSlowEmaPeriod) ||
        settings.trendFastEmaPeriod >= settings.trendSlowEmaPeriod ||
        !validPeriod(settings.fastEmaPeriod) ||
        !validPeriod(settings.slowEmaPeriod) || settings.fastEmaPeriod >= settings.slowEmaPeriod ||
        !validPeriod(settings.rsiPeriod) || !validPeriod(settings.macdSignalPeriod) ||
        !validPeriod(settings.atrPeriod) || !validPeriod(settings.supportResistanceLookback)) {
        return std::nullopt;
    }

    // MACD needs slowPeriod closes for its first line and signalPeriod MACD
    // values for its first signal output. Guard the addition explicitly.
    if (settings.slowEmaPeriod > kMaximumPeriod - settings.macdSignalPeriod + 1) {
        return std::nullopt;
    }
    const auto macdHistory = settings.slowEmaPeriod + settings.macdSignalPeriod - 1;
    if (settings.supportResistanceLookback == kMaximumPeriod) {
        return std::nullopt;
    }
    return std::max({settings.smaPeriod, settings.trendSlowEmaPeriod, settings.slowEmaPeriod,
                     settings.rsiPeriod + 1, macdHistory, settings.atrPeriod + 1,
                     settings.supportResistanceLookback + 1});
}

std::string formatUtc(std::chrono::system_clock::time_point timestamp) {
    const auto time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%d %H:%M:%S UTC");
    return output.str();
}

std::string formatValue(double value) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2) << value;
    return output.str();
}

TechnicalAnalysisResult baseResult(MarketSymbol market,
                                   std::span<const OhlcCandle> candles,
                                   const TechnicalAnalysisSettings& settings) {
    TechnicalAnalysisResult result;
    result.market = market;
    result.symbol = marketSymbolName(market);
    result.candleCount = candles.size();
    result.settings = settings;
    if (const auto required = minimumCandles(settings)) {
        result.minimumRequiredCandles = *required;
    }
    if (!candles.empty()) {
        result.source = candles.front().source;
        result.timeframe = candles.front().interval;
    }
    return result;
}

TechnicalAnalysisResult insufficient(TechnicalAnalysisResult result, std::string reason) {
    result.status = AnalysisStatus::InsufficientData;
    result.reason = std::move(reason);
    return result;
}

TechnicalAnalysisResult invalid(TechnicalAnalysisResult result, std::string reason) {
    result.status = AnalysisStatus::InvalidData;
    result.reason = std::move(reason);
    return result;
}

} // namespace

std::optional<double> TechnicalAnalysis::sma(std::span<const double> values,
                                             std::size_t period) {
    if (period == 0 || period > kMaximumPeriod || values.size() < period) {
        return std::nullopt;
    }
    return average(values.last(period));
}

std::optional<double> TechnicalAnalysis::ema(std::span<const double> values,
                                             std::size_t period) {
    const auto series = emaSeries(values, period);
    if (series.empty() || !std::isfinite(series.back())) {
        return std::nullopt;
    }
    return series.back();
}

std::optional<double> TechnicalAnalysis::rsi(std::span<const double> closes,
                                             std::size_t period) {
    if (period == 0 || period > kMaximumPeriod || closes.size() <= period ||
        !allFinite(closes)) {
        return std::nullopt;
    }

    long double averageGain = 0.0L;
    long double averageLoss = 0.0L;
    for (std::size_t index = 1; index <= period; ++index) {
        const double change = closes[index] - closes[index - 1];
        if (!std::isfinite(change)) {
            return std::nullopt;
        }
        averageGain += static_cast<long double>(std::max(change, 0.0)) /
                       static_cast<long double>(period);
        averageLoss += static_cast<long double>(std::max(-change, 0.0)) /
                       static_cast<long double>(period);
    }

    for (std::size_t index = period + 1; index < closes.size(); ++index) {
        const double change = closes[index] - closes[index - 1];
        if (!std::isfinite(change)) {
            return std::nullopt;
        }
        const long double gain = static_cast<long double>(std::max(change, 0.0));
        const long double loss = static_cast<long double>(std::max(-change, 0.0));
        const long double divisor = static_cast<long double>(period);
        averageGain = (averageGain * (divisor - 1.0L) + gain) / divisor;
        averageLoss = (averageLoss * (divisor - 1.0L) + loss) / divisor;
    }

    double result = 50.0;
    if (averageLoss == 0.0L && averageGain > 0.0L) {
        result = 100.0;
    } else if (averageGain == 0.0L && averageLoss > 0.0L) {
        result = 0.0;
    } else if (averageGain > 0.0L && averageLoss > 0.0L) {
        const long double relativeStrength = averageGain / averageLoss;
        result = static_cast<double>(100.0L - 100.0L / (1.0L + relativeStrength));
    }
    return std::isfinite(result) ? std::optional<double>(result) : std::nullopt;
}

std::optional<MacdValues> TechnicalAnalysis::macd(std::span<const double> closes,
                                                 std::size_t fastPeriod,
                                                 std::size_t slowPeriod,
                                                 std::size_t signalPeriod) {
    if (fastPeriod == 0 || fastPeriod > kMaximumPeriod || slowPeriod == 0 ||
        slowPeriod > kMaximumPeriod || signalPeriod == 0 || signalPeriod > kMaximumPeriod ||
        fastPeriod >= slowPeriod || closes.size() < slowPeriod + signalPeriod - 1) {
        return std::nullopt;
    }
    const auto fast = emaSeries(closes, fastPeriod);
    const auto slow = emaSeries(closes, slowPeriod);
    if (fast.empty() || slow.empty()) {
        return std::nullopt;
    }
    const auto firstMacdIndex = slowPeriod - 1;
    std::vector<double> macdLine;
    macdLine.reserve(closes.size() - firstMacdIndex);
    for (std::size_t index = firstMacdIndex; index < closes.size(); ++index) {
        if (!std::isfinite(fast[index]) || !std::isfinite(slow[index])) {
            return std::nullopt;
        }
        macdLine.push_back(fast[index] - slow[index]);
    }
    const auto signal = ema(macdLine, signalPeriod);
    if (!signal || !std::isfinite(macdLine.back())) {
        return std::nullopt;
    }
    const double histogram = macdLine.back() - *signal;
    if (!std::isfinite(histogram)) {
        return std::nullopt;
    }
    return MacdValues{macdLine.back(), *signal, histogram};
}

std::optional<double> TechnicalAnalysis::atr(std::span<const OhlcCandle> candles,
                                             std::size_t period) {
    if (period == 0 || period > kMaximumPeriod || candles.size() <= period) {
        return std::nullopt;
    }
    long double smoothed = 0.0L;
    for (std::size_t index = 1; index <= period; ++index) {
        const auto& candle = candles[index];
        const auto& previous = candles[index - 1];
        if (!std::isfinite(candle.high) || !std::isfinite(candle.low) ||
            !std::isfinite(previous.close)) {
            return std::nullopt;
        }
        const double trueRange = std::max({candle.high - candle.low,
                                           std::abs(candle.high - previous.close),
                                           std::abs(candle.low - previous.close)});
        if (!std::isfinite(trueRange)) {
            return std::nullopt;
        }
        smoothed += static_cast<long double>(trueRange) / static_cast<long double>(period);
    }

    const long double divisor = static_cast<long double>(period);
    for (std::size_t index = period + 1; index < candles.size(); ++index) {
        const auto& candle = candles[index];
        const double trueRange = std::max({candle.high - candle.low,
                                           std::abs(candle.high - candles[index - 1].close),
                                           std::abs(candle.low - candles[index - 1].close)});
        if (!std::isfinite(trueRange)) {
            return std::nullopt;
        }
        smoothed = (smoothed * (divisor - 1.0L) + static_cast<long double>(trueRange)) /
                   divisor;
    }
    const double result = static_cast<double>(smoothed);
    return std::isfinite(result) ? std::optional<double>(result) : std::nullopt;
}

std::optional<SupportResistanceLevels> TechnicalAnalysis::supportResistance(
    std::span<const OhlcCandle> candles, std::size_t lookback) {
    if (lookback == 0 || candles.size() <= lookback) {
        return std::nullopt;
    }
    const std::size_t end = candles.size() - 1; // Exclude the latest candle.
    const std::size_t begin = end - lookback;
    double support = std::numeric_limits<double>::infinity();
    double resistance = -std::numeric_limits<double>::infinity();
    for (std::size_t index = begin; index < end; ++index) {
        if (!std::isfinite(candles[index].low) || !std::isfinite(candles[index].high)) {
            return std::nullopt;
        }
        support = std::min(support, candles[index].low);
        resistance = std::max(resistance, candles[index].high);
    }
    if (!std::isfinite(support) || !std::isfinite(resistance)) {
        return std::nullopt;
    }
    return SupportResistanceLevels{support, resistance, lookback};
}

TechnicalAnalysisResult TechnicalAnalysis::analyze(
    MarketSymbol market, std::span<const OhlcCandle> candles,
    std::chrono::system_clock::time_point asOf,
    const TechnicalAnalysisSettings& settings) {
    auto result = baseResult(market, candles, settings);
    const auto required = minimumCandles(settings);
    if (!required) {
        return invalid(std::move(result), "analysis periods are invalid");
    }
    if (candles.empty()) {
        return insufficient(std::move(result), "no historical candles were returned");
    }
    const auto timeframe = parseTimeframe(candles.front().interval);
    if (!timeframe) {
        return insufficient(std::move(result),
                            "provider candles do not use a supported fixed interval");
    }
    result.timeframe = marketTimeframeName(*timeframe);
    const auto expectedSymbol = marketSymbolName(market);
    const auto expectedDuration = timeframeDuration(*timeframe);
    const std::string& source = candles.front().source;

    for (std::size_t index = 0; index < candles.size(); ++index) {
        const auto& candle = candles[index];
        if (candle.symbol != expectedSymbol || candle.source.empty() || candle.source != source ||
            parseTimeframe(candle.interval) != timeframe) {
            return invalid(std::move(result), "candle symbol, source, or interval is inconsistent");
        }
        if (candle.startTime.time_since_epoch() <= std::chrono::system_clock::duration::zero() ||
            candle.endTime - candle.startTime != expectedDuration ||
            !std::isfinite(candle.open) || !std::isfinite(candle.high) ||
            !std::isfinite(candle.low) || !std::isfinite(candle.close) || candle.open <= 0.0 ||
            candle.close <= 0.0 || candle.low <= 0.0 || candle.high <= 0.0 ||
            candle.high < std::max(candle.open, candle.close) || candle.high < candle.low ||
            candle.low > std::min(candle.open, candle.close)) {
            return invalid(std::move(result), "candle timestamps or OHLC values are invalid");
        }
        if (candle.endTime > asOf) {
            return insufficient(std::move(result), "the latest candle is not complete yet");
        }
        if (index > 0) {
            if (candle.startTime < candles[index - 1].endTime) {
                return invalid(std::move(result), "candle timestamps overlap or are out of order");
            }
            if (candle.startTime != candles[index - 1].endTime) {
                return insufficient(std::move(result), "historical candles contain a time gap");
            }
        }
    }

    result.source = source;
    result.latestCandleEnd = candles.back().endTime;
    result.delayed = candles.back().delayed;
    if (candles.size() < *required) {
        return insufficient(std::move(result), "not enough complete candles for all indicators");
    }

    std::vector<double> closes;
    closes.reserve(candles.size());
    for (const auto& candle : candles) {
        closes.push_back(candle.close);
    }
    result.latestClose = closes.back();
    result.sma = sma(closes, settings.smaPeriod);
    result.ema20 = ema(closes, settings.trendFastEmaPeriod);
    result.ema50 = ema(closes, settings.trendSlowEmaPeriod);
    result.emaFast = ema(closes, settings.fastEmaPeriod);
    result.emaSlow = ema(closes, settings.slowEmaPeriod);
    result.rsi = rsi(closes, settings.rsiPeriod);
    result.macd = macd(closes, settings.fastEmaPeriod, settings.slowEmaPeriod,
                       settings.macdSignalPeriod);
    result.atr = atr(candles, settings.atrPeriod);
    result.supportResistance = supportResistance(candles, settings.supportResistanceLookback);
    if (!result.sma || !result.ema20 || !result.ema50 || !result.emaFast ||
        !result.emaSlow || !result.rsi || !result.macd ||
        !result.atr || !result.supportResistance) {
        return invalid(std::move(result), "indicator calculation failed validation");
    }

    const double latestClose = closes.back();
    const double macdTolerance = 1e-12 *
        std::max({1.0, std::abs(result.macd->line), std::abs(result.macd->signal)});
    if (latestClose > *result.ema20 && *result.ema20 > *result.ema50 &&
        result.macd->line + macdTolerance >= result.macd->signal && *result.rsi > 50.0) {
        result.trend = TrendClassification::Bullish;
    } else if (latestClose < *result.ema20 && *result.ema20 < *result.ema50 &&
               result.macd->line <= result.macd->signal + macdTolerance && *result.rsi < 50.0) {
        result.trend = TrendClassification::Bearish;
    } else {
        result.trend = TrendClassification::Neutral;
    }
    result.status = AnalysisStatus::Complete;
    return result;
}

std::string marketTimeframeName(MarketTimeframe timeframe) {
    switch (timeframe) {
    case MarketTimeframe::M5:
        return "5m";
    case MarketTimeframe::M15:
        return "15m";
    case MarketTimeframe::H1:
        return "1h";
    case MarketTimeframe::M30:
        return "30m";
    case MarketTimeframe::H4:
        return "4h";
    case MarketTimeframe::D4:
        return "4d";
    }
    return "unknown";
}

std::string analysisStatusName(AnalysisStatus status) {
    switch (status) {
    case AnalysisStatus::Complete:
        return "COMPLETE";
    case AnalysisStatus::InsufficientData:
        return "INSUFFICIENT DATA";
    case AnalysisStatus::InvalidData:
        return "DATA UNAVAILABLE";
    }
    return "DATA UNAVAILABLE";
}

std::string trendClassificationName(TrendClassification trend) {
    switch (trend) {
    case TrendClassification::Bullish:
        return "BULLISH";
    case TrendClassification::Bearish:
        return "BEARISH";
    case TrendClassification::Neutral:
        return "NEUTRAL";
    }
    return "NEUTRAL";
}

std::string formatTechnicalAnalysisMessage(const TechnicalAnalysisResult& result) {
    std::ostringstream output;
    output << marketDisplayName(result.market) << " (" << result.symbol << ") technical analysis\n";
    output << "Status: " << analysisStatusName(result.status) << '\n';
    if (!result.timeframe.empty()) {
        output << "Timeframe: " << result.timeframe << '\n';
    }
    if (!result.source.empty()) {
        output << "Source: " << result.source << '\n';
    }
    output << "Candles: " << result.candleCount;
    if (result.minimumRequiredCandles > 0) {
        output << " (" << result.minimumRequiredCandles << " required)";
    }
    output << '\n';
    if (result.status != AnalysisStatus::Complete) {
        if (!result.reason.empty()) {
            output << "Reason: " << result.reason << '\n';
        }
        return output.str();
    }

    output << "Latest completed candle: " << formatUtc(result.latestCandleEnd) << '\n'
           << "Latest close: " << formatValue(*result.latestClose) << '\n'
           << "Trend classification: " << trendClassificationName(result.trend) << '\n'
           << "SMA(" << result.settings.smaPeriod << "): " << formatValue(*result.sma) << '\n'
           << "EMA(20): " << formatValue(*result.ema20) << '\n'
           << "EMA(50): " << formatValue(*result.ema50) << '\n'
           << "EMA(" << result.settings.fastEmaPeriod << "): "
           << formatValue(*result.emaFast) << '\n'
           << "EMA(" << result.settings.slowEmaPeriod << "): "
           << formatValue(*result.emaSlow) << '\n'
           << "RSI(" << result.settings.rsiPeriod << "): " << formatValue(*result.rsi) << '\n'
           << "MACD(" << result.settings.fastEmaPeriod << ','
           << result.settings.slowEmaPeriod << ',' << result.settings.macdSignalPeriod << "): "
           << formatValue(result.macd->line)
           << " / signal " << formatValue(result.macd->signal)
           << " / histogram " << formatValue(result.macd->histogram) << '\n'
           << "ATR(" << result.settings.atrPeriod << "): " << formatValue(*result.atr) << '\n'
           << "Prior " << result.supportResistance->lookbackCandles
           << " candle support/resistance: " << formatValue(result.supportResistance->support)
           << " / " << formatValue(result.supportResistance->resistance);
    if (result.delayed) {
        output << "\nDATA DELAYED: latest completed candle is marked delayed by its provider.";
    }
    output << "\nDescriptive market analysis only; no trading instruction.";
    return output.str();
}

} // namespace trading_bot
