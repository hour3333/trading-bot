#pragma once

#include "trading_bot/market_data.hpp"

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace trading_bot {

enum class BacktestStatus { Complete, InsufficientData, InvalidData };
enum class SimulatedDirection { Long, Short };

struct BacktestSettings {
    double startingBalance{10'000.0};
    double feeBasisPoints{10.0};
    double slippageBasisPoints{5.0};
    double riskFraction{0.01};
    double minimumRiskReward{1.5};
};

struct SimulatedTrade {
    MarketSymbol market{MarketSymbol::BtcUsd};
    std::string source;
    std::string timeframe;
    SimulatedDirection direction{SimulatedDirection::Long};
    std::chrono::system_clock::time_point entryTime{};
    std::chrono::system_clock::time_point exitTime{};
    double entryPrice{0.0};
    double averageExitPrice{0.0};
    double stopLoss{0.0};
    double target1{0.0};
    double target2{0.0};
    double quantity{0.0};
    double grossPnl{0.0};
    double feesPaid{0.0};
    double netPnl{0.0};
    double riskAmount{0.0};
    double realizedRiskReward{0.0};
    bool target1Reached{false};
    std::string exitReason;
};

struct BacktestResult {
    std::string runKey;
    MarketSymbol market{MarketSymbol::BtcUsd};
    BacktestStatus status{BacktestStatus::InsufficientData};
    std::string source;
    std::string timeframe;
    std::string reason;
    std::chrono::system_clock::time_point historyStart{};
    std::chrono::system_clock::time_point historyEnd{};
    std::size_t candleCount{0};
    std::size_t minimumRequiredCandles{35};
    std::size_t decisionBars{0};
    BacktestSettings settings;
    double endingBalance{0.0};
    double netPnl{0.0};
    double totalFees{0.0};
    std::size_t wins{0};
    std::size_t losses{0};
    std::optional<double> winRate;
    std::optional<double> averageRiskReward;
    double maximumDrawdown{0.0};
    double maximumDrawdownPercent{0.0};
    std::vector<SimulatedTrade> trades;
};

class BacktestEngine {
public:
    // Executes only against the provided, completed historical OHLC sequence.
    // The current bar's close creates a scenario; the next bar's open is the
    // earliest possible entry. Same-bar stop/target ambiguity is resolved stop-first.
    [[nodiscard]] static BacktestResult run(
        MarketSymbol market, std::span<const OhlcCandle> candles,
        const BacktestSettings& settings = {},
        std::chrono::system_clock::time_point asOf = std::chrono::system_clock::now());
};

[[nodiscard]] std::string backtestStatusName(BacktestStatus status);
[[nodiscard]] std::string simulatedDirectionName(SimulatedDirection direction);
[[nodiscard]] std::string formatBacktestPerformance(const BacktestResult& result);
[[nodiscard]] std::string formatBacktestPerformanceMessage(
    std::span<const BacktestResult> results);
[[nodiscard]] std::string formatBacktestHistory(
    std::span<const SimulatedTrade> trades, std::size_t totalCount);

} // namespace trading_bot
