#pragma once

#include "trading_bot/market_data.hpp"
#include "trading_bot/technical_analysis.hpp"

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace trading_bot {

enum class SignalState { BuySetup, SellSetup, Wait };
enum class SignalDataStatus { Ready, DataUnavailable, InsufficientData, InvalidData, StaleData };
enum class SignalConfidence { High, Medium, Low, Unconfirmed };
enum class SignalRiskLevel { Low, Medium, High };

struct SignalScenario {
    MarketSymbol market{MarketSymbol::BtcUsd};
    std::chrono::system_clock::time_point analysisAt{};
    std::string timeframe;
    TrendClassification trend{TrendClassification::Neutral};
    SignalState state{SignalState::Wait};
    SignalDataStatus dataStatus{SignalDataStatus::InsufficientData};
    std::optional<double> quotePrice;
    std::optional<double> latestCandleClose;
    std::optional<double> sma20;
    std::optional<double> ema20;
    std::optional<double> ema50;
    std::optional<double> emaFast;
    std::optional<double> emaSlow;
    std::optional<double> rsi14;
    std::optional<double> macdHistogram;
    std::optional<double> atr;
    std::optional<double> support;
    std::optional<double> resistance;
    std::optional<double> entryZoneLow;
    std::optional<double> entryZoneHigh;
    std::optional<double> stopLoss;
    std::optional<double> takeProfit1;
    std::optional<double> takeProfit2;
    std::optional<double> riskReward1;
    std::optional<double> riskReward2;
    double minimumRiskReward{1.5};
    SignalConfidence confidence{SignalConfidence::Unconfirmed};
    std::string confidenceReason{"Required provider data is unavailable or unconfirmed."};
    SignalRiskLevel riskLevel{SignalRiskLevel::High};
    std::string riskReason{"Risk cannot be assessed without validated market data."};
    std::string dataSource;
    std::optional<std::chrono::system_clock::time_point> quoteTimestamp;
    std::optional<std::chrono::system_clock::time_point> candleTimestamp;
    std::vector<std::string> reasons;
    std::vector<std::string> conflicts;
};

class SignalEngine {
public:
    // Evaluation time is injectable so freshness rules are deterministic in tests.
    [[nodiscard]] static SignalScenario evaluate(
        const MarketDataSnapshot& snapshot,
        std::chrono::system_clock::time_point asOf = std::chrono::system_clock::now());

    // Live educational signals accept only M5, M15, H1, and H4 and require
    // candles matching the requested interval. Missing/short history returns WAIT.
    [[nodiscard]] static SignalScenario evaluate(
        const MarketDataSnapshot& snapshot, std::string_view timeframe,
        std::chrono::system_clock::time_point asOf = std::chrono::system_clock::now(),
        double minimumRiskReward = 1.5);

    // Causal replay helper. The supplied span must end at the candle known at
    // the simulated decision time; no later candles are inspected. The latest
    // candle close is used as the historical reference price, never as a live quote.
    [[nodiscard]] static SignalScenario evaluateHistorical(
        MarketSymbol market, std::span<const OhlcCandle> observedCandles,
        double minimumRiskReward = 1.5);
};

[[nodiscard]] std::string signalStateName(SignalState state);
[[nodiscard]] std::string signalDataStatusName(SignalDataStatus status);
[[nodiscard]] std::string signalConfidenceName(SignalConfidence confidence);
[[nodiscard]] std::string signalRiskLevelName(SignalRiskLevel risk);
// Returns a validation issue when the scenario contains inconsistent states,
// levels, or risk/reward ratios.
[[nodiscard]] std::optional<std::string> validateSignalScenario(
    const SignalScenario& scenario);
[[nodiscard]] std::string formatSignalScenario(
    const SignalScenario& scenario, std::string_view timezone = "Asia/Phnom_Penh");

} // namespace trading_bot
