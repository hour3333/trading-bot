#pragma once

#include "trading_bot/market_data.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace trading_bot {

enum class MarketTimeframe { M5, M15, H1, M30, H4, D4 };
enum class AnalysisStatus { Complete, InsufficientData, InvalidData };
enum class TrendClassification { Bullish, Bearish, Neutral };

struct MacdValues {
    double line{0.0};
    double signal{0.0};
    double histogram{0.0};
};

struct SupportResistanceLevels {
    double support{0.0};
    double resistance{0.0};
    std::size_t lookbackCandles{0};
};

struct TechnicalAnalysisSettings {
    std::size_t smaPeriod{20};
    std::size_t trendFastEmaPeriod{20};
    std::size_t trendSlowEmaPeriod{50};
    std::size_t fastEmaPeriod{12};
    std::size_t slowEmaPeriod{26};
    std::size_t rsiPeriod{14};
    std::size_t macdSignalPeriod{9};
    std::size_t atrPeriod{14};
    std::size_t supportResistanceLookback{20};
};

struct TechnicalAnalysisResult {
    MarketSymbol market{MarketSymbol::BtcUsd};
    AnalysisStatus status{AnalysisStatus::InsufficientData};
    std::string symbol;
    std::string source;
    std::string timeframe;
    std::chrono::system_clock::time_point latestCandleEnd{};
    std::optional<double> latestClose;
    std::size_t candleCount{0};
    std::size_t minimumRequiredCandles{0};
    TechnicalAnalysisSettings settings;
    std::string reason;
    std::optional<double> sma;
    std::optional<double> ema20;
    std::optional<double> ema50;
    std::optional<double> emaFast;
    std::optional<double> emaSlow;
    std::optional<double> rsi;
    std::optional<MacdValues> macd;
    std::optional<double> atr;
    std::optional<SupportResistanceLevels> supportResistance;
    TrendClassification trend{TrendClassification::Neutral};
    bool delayed{false};
};

// Indicators use the documented SMA-seeded EMA convention and Wilder smoothing
// for RSI and ATR. All functions return nullopt when their input is insufficient
// or contains non-finite values.
class TechnicalAnalysis {
public:
    [[nodiscard]] static std::optional<double> sma(std::span<const double> values,
                                                    std::size_t period);
    [[nodiscard]] static std::optional<double> ema(std::span<const double> values,
                                                    std::size_t period);
    [[nodiscard]] static std::optional<double> rsi(std::span<const double> closes,
                                                    std::size_t period = 14);
    [[nodiscard]] static std::optional<MacdValues> macd(
        std::span<const double> closes, std::size_t fastPeriod = 12,
        std::size_t slowPeriod = 26, std::size_t signalPeriod = 9);
    [[nodiscard]] static std::optional<double> atr(std::span<const OhlcCandle> candles,
                                                    std::size_t period = 14);
    // The latest candle is excluded so the level is based only on prior bars.
    [[nodiscard]] static std::optional<SupportResistanceLevels> supportResistance(
        std::span<const OhlcCandle> candles, std::size_t lookback = 20);

    [[nodiscard]] static TechnicalAnalysisResult analyze(
        MarketSymbol market, std::span<const OhlcCandle> candles,
        std::chrono::system_clock::time_point asOf = std::chrono::system_clock::now(),
        const TechnicalAnalysisSettings& settings = {});
};

[[nodiscard]] std::string marketTimeframeName(MarketTimeframe timeframe);
[[nodiscard]] std::string analysisStatusName(AnalysisStatus status);
[[nodiscard]] std::string trendClassificationName(TrendClassification trend);
[[nodiscard]] std::string formatTechnicalAnalysisMessage(const TechnicalAnalysisResult& result);

} // namespace trading_bot
