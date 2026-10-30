#pragma once

#include "trading_bot/market_data.hpp"
#include "trading_bot/technical_analysis.hpp"

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace trading_bot {

enum class SignalState { BuySetup, SellSetup, Wait };
enum class SignalDataStatus { Ready, InsufficientData, InvalidData, StaleData };

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
    std::optional<double> emaFast;
    std::optional<double> emaSlow;
    std::optional<double> rsi14;
    std::optional<double> macdHistogram;
    std::optional<double> atr;
    std::optional<double> entryZoneLow;
    std::optional<double> entryZoneHigh;
    std::optional<double> stopLoss;
    std::optional<double> takeProfit1;
    std::optional<double> takeProfit2;
    std::optional<double> riskReward1;
    std::optional<double> riskReward2;
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

    // Causal replay helper. The supplied span must end at the candle known at
    // the simulated decision time; no later candles are inspected. The latest
    // candle close is used as the historical reference price, never as a live quote.
    [[nodiscard]] static SignalScenario evaluateHistorical(
        MarketSymbol market, std::span<const OhlcCandle> observedCandles);
};

[[nodiscard]] std::string signalStateName(SignalState state);
[[nodiscard]] std::string signalDataStatusName(SignalDataStatus status);
[[nodiscard]] std::string formatSignalScenario(const SignalScenario& scenario);

} // namespace trading_bot
