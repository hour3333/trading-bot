#include "trading_bot/backtest_engine.hpp"

#include "trading_bot/signal_engine.hpp"

#include <algorithm>
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
constexpr std::size_t kSignalWarmupCandles = 50;
constexpr std::size_t kMinimumCandles = kSignalWarmupCandles + 1;
constexpr double kMaximumFeeBasisPoints = 1000.0;
constexpr double kMaximumSlippageBasisPoints = 1000.0;
constexpr double kMaximumRiskFraction = 0.10;

struct Position {
    SimulatedDirection direction{SimulatedDirection::Long};
    std::string source;
    std::string timeframe;
    Clock::time_point entryTime{};
    double entryPrice{0.0};
    double initialQuantity{0.0};
    double remainingQuantity{0.0};
    double stopLoss{0.0};
    double target1{0.0};
    double target2{0.0};
    double riskAmount{0.0};
    double entryFee{0.0};
    double grossPnl{0.0};
    double exitFees{0.0};
    double exitNotional{0.0};
    double exitedQuantity{0.0};
    bool target1Reached{false};
    std::string exitReason;
};

std::optional<Clock::duration> intervalDuration(std::string_view interval) {
    using namespace std::chrono;
    if (interval == "5m") {
        return minutes(5);
    }
    if (interval == "15m") {
        return minutes(15);
    }
    if (interval == "1h") {
        return hours(1);
    }
    if (interval == "30m") {
        return minutes(30);
    }
    if (interval == "4h") {
        return hours(4);
    }
    if (interval == "4d") {
        return days(4);
    }
    return std::nullopt;
}

bool validSettings(const BacktestSettings& settings) {
    return std::isfinite(settings.startingBalance) && settings.startingBalance > 0.0 &&
           settings.startingBalance <= 1.0e12 &&
           std::isfinite(settings.feeBasisPoints) && settings.feeBasisPoints >= 0.0 &&
           settings.feeBasisPoints <= kMaximumFeeBasisPoints &&
           std::isfinite(settings.slippageBasisPoints) && settings.slippageBasisPoints >= 0.0 &&
           settings.slippageBasisPoints <= kMaximumSlippageBasisPoints &&
           std::isfinite(settings.riskFraction) && settings.riskFraction > 0.0 &&
           settings.riskFraction <= kMaximumRiskFraction &&
           std::isfinite(settings.minimumRiskReward) && settings.minimumRiskReward >= 0.5 &&
           settings.minimumRiskReward <= 10.0;
}

std::uint64_t fnv1a(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string makeRunKey(MarketSymbol market, std::span<const OhlcCandle> candles,
                       const BacktestSettings& settings) {
    std::ostringstream material;
    material << marketSymbolName(market) << '|' << std::setprecision(17)
             << settings.startingBalance << '|' << settings.feeBasisPoints << '|'
             << settings.slippageBasisPoints << '|' << settings.riskFraction << '|'
             << settings.minimumRiskReward;
    for (const auto& candle : candles) {
        material << '|' << candle.symbol << '|' << candle.source << '|' << candle.interval
                 << '|' << candle.startTime.time_since_epoch().count()
                 << '|' << candle.endTime.time_since_epoch().count()
                 << '|' << candle.open << '|' << candle.high << '|' << candle.low << '|'
                 << candle.close;
    }
    std::ostringstream key;
    key << "backtest-v1-" << std::hex << fnv1a(material.str());
    return key.str();
}

BacktestResult baseResult(MarketSymbol market, std::span<const OhlcCandle> candles,
                          const BacktestSettings& settings) {
    BacktestResult result;
    result.market = market;
    result.settings = settings;
    result.candleCount = candles.size();
    result.endingBalance = settings.startingBalance;
    result.runKey = makeRunKey(market, candles, settings);
    if (!candles.empty()) {
        result.source = candles.front().source;
        result.timeframe = candles.front().interval;
        result.historyStart = candles.front().startTime;
        result.historyEnd = candles.back().endTime;
    }
    return result;
}

BacktestResult invalid(BacktestResult result, std::string reason) {
    result.status = BacktestStatus::InvalidData;
    result.reason = std::move(reason);
    return result;
}

BacktestResult insufficient(BacktestResult result, std::string reason) {
    result.status = BacktestStatus::InsufficientData;
    result.reason = std::move(reason);
    return result;
}

bool validateCandles(MarketSymbol market, std::span<const OhlcCandle> candles,
                     Clock::time_point asOf, std::string& reason,
                     bool& incomplete) {
    if (candles.empty()) {
        reason = "no historical OHLC candles were returned";
        incomplete = true;
        return false;
    }
    const auto duration = intervalDuration(candles.front().interval);
    if (!duration) {
        reason = "historical candles do not use a supported fixed interval";
        incomplete = true;
        return false;
    }
    const std::string expectedSymbol = marketSymbolName(market);
    const std::string expectedSource = candles.front().source;
    for (std::size_t index = 0; index < candles.size(); ++index) {
        const auto& candle = candles[index];
        if (candle.symbol != expectedSymbol || candle.source.empty() ||
            candle.source != expectedSource || candle.interval != candles.front().interval) {
            reason = "candle symbol, source, or interval is inconsistent";
            return false;
        }
        if (candle.startTime.time_since_epoch() <= Clock::duration::zero() ||
            candle.endTime - candle.startTime != *duration ||
            !std::isfinite(candle.open) || !std::isfinite(candle.high) ||
            !std::isfinite(candle.low) || !std::isfinite(candle.close) ||
            candle.open <= 0.0 || candle.high <= 0.0 || candle.low <= 0.0 ||
            candle.close <= 0.0 || candle.high < std::max(candle.open, candle.close) ||
            candle.high < candle.low || candle.low > std::min(candle.open, candle.close)) {
            reason = "candle timestamps or OHLC values are invalid";
            return false;
        }
        if (candle.endTime > asOf) {
            reason = "history contains a candle that was not complete at the replay time";
            incomplete = true;
            return false;
        }
        if (index > 0) {
            if (candle.startTime < candles[index - 1].endTime) {
                reason = "candle timestamps overlap or are out of order";
                return false;
            }
            if (candle.startTime != candles[index - 1].endTime) {
                reason = "historical candles contain a time gap";
                incomplete = true;
                return false;
            }
        }
    }
    return true;
}

double adverseEntryFill(SimulatedDirection direction, double reference,
                        double slippageRate) {
    return direction == SimulatedDirection::Long
               ? reference * (1.0 + slippageRate)
               : reference * (1.0 - slippageRate);
}

double adverseExitFill(SimulatedDirection direction, double reference,
                       double slippageRate) {
    return direction == SimulatedDirection::Long
               ? reference * (1.0 - slippageRate)
               : reference * (1.0 + slippageRate);
}

void markDrawdown(BacktestResult& result, double equity, double& peakEquity) {
    if (!std::isfinite(equity)) {
        return;
    }
    peakEquity = std::max(peakEquity, equity);
    const double drawdown = std::max(0.0, peakEquity - equity);
    result.maximumDrawdown = std::max(result.maximumDrawdown, drawdown);
    if (peakEquity > 0.0) {
        result.maximumDrawdownPercent = std::max(
            result.maximumDrawdownPercent, drawdown / peakEquity * 100.0);
    }
}

void executeExit(Position& position, double quantity, double referencePrice,
                 BacktestResult& result, double& balance, double feeRate,
                 double slippageRate, std::string reason) {
    quantity = std::min(quantity, position.remainingQuantity);
    if (quantity <= 0.0) {
        return;
    }
    const double fill = adverseExitFill(position.direction, referencePrice, slippageRate);
    const double fee = quantity * fill * feeRate;
    const double gross = position.direction == SimulatedDirection::Long
                             ? quantity * (fill - position.entryPrice)
                             : quantity * (position.entryPrice - fill);
    position.grossPnl += gross;
    position.exitFees += fee;
    position.exitNotional += quantity * fill;
    position.exitedQuantity += quantity;
    position.remainingQuantity -= quantity;
    balance += gross - fee;
    result.totalFees += fee;
    position.exitReason = std::move(reason);
}

bool closed(const Position& position) {
    return position.remainingQuantity <= position.initialQuantity * 1e-12;
}

void finishTrade(Position& position, MarketSymbol market, Clock::time_point exitTime,
                 BacktestResult& result) {
    if (!closed(position) || position.exitedQuantity <= 0.0) {
        return;
    }
    const double netPnl = position.grossPnl - position.entryFee - position.exitFees;
    SimulatedTrade trade;
    trade.market = market;
    trade.source = position.source;
    trade.timeframe = position.timeframe;
    trade.direction = position.direction;
    trade.entryTime = position.entryTime;
    trade.exitTime = exitTime;
    trade.entryPrice = position.entryPrice;
    trade.averageExitPrice = position.exitNotional / position.exitedQuantity;
    trade.stopLoss = position.stopLoss;
    trade.target1 = position.target1;
    trade.target2 = position.target2;
    trade.quantity = position.initialQuantity;
    trade.grossPnl = position.grossPnl;
    trade.feesPaid = position.entryFee + position.exitFees;
    trade.netPnl = netPnl;
    trade.riskAmount = position.riskAmount;
    trade.realizedRiskReward = position.riskAmount > 0.0 ? netPnl / position.riskAmount : 0.0;
    trade.target1Reached = position.target1Reached;
    trade.exitReason = position.exitReason;
    result.trades.push_back(std::move(trade));
    if (netPnl > 0.0) {
        ++result.wins;
    } else {
        ++result.losses;
    }
}

void processPositionBar(Position& position, const OhlcCandle& candle,
                        BacktestResult& result, double& balance,
                        double feeRate, double slippageRate) {
    const bool isLong = position.direction == SimulatedDirection::Long;
    const bool gapsThroughStop = isLong ? candle.open <= position.stopLoss
                                        : candle.open >= position.stopLoss;
    if (gapsThroughStop) {
        const auto reason = position.target1Reached ? "TP1_THEN_STOP_LOSS" : "STOP_LOSS";
        executeExit(position, position.remainingQuantity, candle.open, result, balance,
                    feeRate, slippageRate, reason);
        return;
    }

    const bool opensThroughTarget1 = isLong ? candle.open >= position.target1
                                            : candle.open <= position.target1;
    const bool opensThroughTarget2 = isLong ? candle.open >= position.target2
                                            : candle.open <= position.target2;
    if (!position.target1Reached && opensThroughTarget1) {
        executeExit(position, position.initialQuantity * 0.5, position.target1, result,
                    balance, feeRate, slippageRate, "TP1_PARTIAL");
        position.target1Reached = true;
    }
    if (opensThroughTarget2) {
        executeExit(position, position.remainingQuantity, position.target2, result,
                    balance, feeRate, slippageRate, "TP1_AND_TP2");
        return;
    }

    const bool touchesStop = isLong ? candle.low <= position.stopLoss
                                    : candle.high >= position.stopLoss;
    const bool touchesTarget1 = isLong ? candle.high >= position.target1
                                       : candle.low <= position.target1;
    const bool touchesTarget2 = isLong ? candle.high >= position.target2
                                       : candle.low <= position.target2;
    if (touchesStop) {
        // OHLC does not reveal which level came first. A bar that also touches
        // a target is resolved against the simulated position.
        const auto reason = position.target1Reached ? "TP1_THEN_STOP_LOSS" : "STOP_LOSS";
        executeExit(position, position.remainingQuantity, position.stopLoss, result,
                    balance, feeRate, slippageRate, reason);
        return;
    }

    if (!position.target1Reached && touchesTarget1) {
        executeExit(position, position.initialQuantity * 0.5, position.target1, result,
                    balance, feeRate, slippageRate, "TP1_PARTIAL");
        position.target1Reached = true;
    }
    if (touchesTarget2) {
        executeExit(position, position.remainingQuantity, position.target2, result,
                    balance, feeRate, slippageRate, "TP1_AND_TP2");
    }
}

void updateMetrics(BacktestResult& result) {
    result.netPnl = result.endingBalance - result.settings.startingBalance;
    if (result.trades.empty()) {
        result.winRate.reset();
        result.averageRiskReward.reset();
        return;
    }
    result.winRate = static_cast<double>(result.wins) /
                     static_cast<double>(result.trades.size()) * 100.0;
    double totalR = 0.0;
    for (const auto& trade : result.trades) {
        totalR += trade.realizedRiskReward;
    }
    result.averageRiskReward = totalR / static_cast<double>(result.trades.size());
}

std::string formatUtc(Clock::time_point timestamp) {
    if (timestamp.time_since_epoch() <= Clock::duration::zero()) {
        return "DATA UNAVAILABLE";
    }
    const auto time = Clock::to_time_t(timestamp);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%d %H:%M UTC");
    return output.str();
}

std::string money(double value) {
    std::ostringstream output;
    output << (value >= 0.0 ? "+$" : "-$") << std::fixed << std::setprecision(2)
           << std::abs(value);
    return output.str();
}

std::string number(double value, int precision = 2) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(precision) << value;
    return output.str();
}

} // namespace

BacktestResult BacktestEngine::run(MarketSymbol market, std::span<const OhlcCandle> candles,
                                   const BacktestSettings& settings, Clock::time_point asOf) {
    auto result = baseResult(market, candles, settings);
    if (!validSettings(settings)) {
        return invalid(std::move(result),
                       "backtest balance, costs, risk, or minimum risk/reward settings are invalid");
    }

    std::string validationReason;
    bool incomplete = false;
    if (!validateCandles(market, candles, asOf, validationReason, incomplete)) {
        return incomplete ? insufficient(std::move(result), std::move(validationReason))
                          : invalid(std::move(result), std::move(validationReason));
    }
    if (candles.size() < kMinimumCandles) {
        result.minimumRequiredCandles = kMinimumCandles;
        return insufficient(std::move(result),
                            "at least 50 indicator warm-up candles and one later execution candle are required");
    }

    result.status = BacktestStatus::Complete;
    result.historyStart = candles.front().startTime;
    result.historyEnd = candles.back().endTime;
    result.source = candles.front().source;
    result.timeframe = candles.front().interval;
    result.decisionBars = candles.size() - kSignalWarmupCandles;

    const double feeRate = settings.feeBasisPoints / 10'000.0;
    const double slippageRate = settings.slippageBasisPoints / 10'000.0;
    double balance = settings.startingBalance;
    double peakEquity = balance;
    std::optional<Position> position;

    for (std::size_t executionIndex = kSignalWarmupCandles;
         executionIndex < candles.size(); ++executionIndex) {
        const auto& executionCandle = candles[executionIndex];
        if (!position && balance > 0.0) {
            // The decision sees only candles before this execution candle.
            const auto scenario = SignalEngine::evaluateHistorical(
                market, candles.first(executionIndex), settings.minimumRiskReward);
            if (scenario.dataStatus == SignalDataStatus::Ready &&
                scenario.state != SignalState::Wait && scenario.entryZoneLow &&
                scenario.entryZoneHigh && scenario.stopLoss && scenario.takeProfit1 &&
                scenario.takeProfit2) {
                const auto direction = scenario.state == SignalState::BuySetup
                                           ? SimulatedDirection::Long
                                           : SimulatedDirection::Short;
                const double open = executionCandle.open;
                if (open >= *scenario.entryZoneLow && open <= *scenario.entryZoneHigh) {
                    const double fill = adverseEntryFill(direction, open, slippageRate);
                    const double boundaryTolerance = std::max(1.0, fill) * 1e-12;
                    const bool fillRemainsInZone = fill >= *scenario.entryZoneLow - boundaryTolerance &&
                                                   fill <= *scenario.entryZoneHigh + boundaryTolerance;
                    const bool levelsValid = direction == SimulatedDirection::Long
                        ? fill > *scenario.stopLoss && fill < *scenario.takeProfit1 &&
                          *scenario.takeProfit1 < *scenario.takeProfit2
                        : fill < *scenario.stopLoss && fill > *scenario.takeProfit1 &&
                          *scenario.takeProfit1 > *scenario.takeProfit2;
                    const double stopDistance = std::abs(fill - *scenario.stopLoss);
                    if (fillRemainsInZone && levelsValid && std::isfinite(stopDistance) &&
                        stopDistance > 0.0) {
                        const double riskQuantity = balance * settings.riskFraction / stopDistance;
                        const double accountQuantity = balance / (fill * (1.0 + feeRate));
                        const double quantity = std::min(riskQuantity, accountQuantity);
                        const double entryFee = quantity * fill * feeRate;
                        if (std::isfinite(quantity) && quantity > 0.0 &&
                            std::isfinite(entryFee) && entryFee < balance) {
                            Position opened;
                            opened.direction = direction;
                            opened.source = scenario.dataSource;
                            opened.timeframe = scenario.timeframe;
                            opened.entryTime = executionCandle.startTime;
                            opened.entryPrice = fill;
                            opened.initialQuantity = quantity;
                            opened.remainingQuantity = quantity;
                            opened.stopLoss = *scenario.stopLoss;
                            opened.target1 = *scenario.takeProfit1;
                            opened.target2 = *scenario.takeProfit2;
                            opened.riskAmount = quantity * stopDistance;
                            opened.entryFee = entryFee;
                            balance -= entryFee;
                            result.totalFees += entryFee;
                            position = std::move(opened);
                        }
                    }
                }
            }
        }

        if (position) {
            const bool opensThroughStop = position->direction == SimulatedDirection::Long
                ? executionCandle.open <= position->stopLoss
                : executionCandle.open >= position->stopLoss;
            if (!opensThroughStop) {
                const double adversePrice = position->direction == SimulatedDirection::Long
                    ? std::max(executionCandle.low, position->stopLoss)
                    : std::min(executionCandle.high, position->stopLoss);
                const double direction = position->direction == SimulatedDirection::Long ? 1.0 : -1.0;
                const double adverseEquity = balance + direction *
                    (adversePrice - position->entryPrice) * position->remainingQuantity;
                markDrawdown(result, adverseEquity, peakEquity);
            }
            processPositionBar(*position, executionCandle, result, balance, feeRate,
                               slippageRate);
            if (closed(*position)) {
                finishTrade(*position, market, executionCandle.endTime, result);
                position.reset();
            }
        }

        double equity = balance;
        if (position) {
            const double direction = position->direction == SimulatedDirection::Long ? 1.0 : -1.0;
            equity += direction * (executionCandle.close - position->entryPrice) *
                      position->remainingQuantity;
        }
        markDrawdown(result, equity, peakEquity);
    }

    if (position) {
        const auto reason = position->target1Reached ? "TP1_THEN_END_OF_DATA" : "END_OF_DATA";
        executeExit(*position, position->remainingQuantity, candles.back().close, result,
                    balance, feeRate, slippageRate, reason);
        finishTrade(*position, market, candles.back().endTime, result);
        markDrawdown(result, balance, peakEquity);
    }

    result.endingBalance = balance;
    updateMetrics(result);
    return result;
}

std::string backtestStatusName(BacktestStatus status) {
    switch (status) {
    case BacktestStatus::Complete:
        return "COMPLETE";
    case BacktestStatus::InsufficientData:
        return "INSUFFICIENT DATA";
    case BacktestStatus::InvalidData:
        return "DATA UNAVAILABLE";
    }
    return "DATA UNAVAILABLE";
}

std::string simulatedDirectionName(SimulatedDirection direction) {
    return direction == SimulatedDirection::Long ? "LONG" : "SHORT";
}

std::string formatBacktestPerformance(const BacktestResult& result) {
    std::ostringstream output;
    output << marketDisplayName(result.market) << " (" << marketSymbolName(result.market) << ")\n"
           << "Status: " << backtestStatusName(result.status) << '\n'
           << "Historical source: " << (result.source.empty() ? "DATA UNAVAILABLE" : result.source) << '\n'
           << "Timeframe: " << (result.timeframe.empty() ? "DATA UNAVAILABLE" : result.timeframe) << '\n'
           << "History: " << formatUtc(result.historyStart) << " to " << formatUtc(result.historyEnd) << '\n'
           << "Candles: " << result.candleCount << " | warm-up: " << kSignalWarmupCandles
           << " | replay decisions: " << result.decisionBars << '\n';
    if (result.status != BacktestStatus::Complete) {
        output << "Sample: 0 simulated trades\n"
               << "Performance metrics: " << backtestStatusName(result.status) << '\n'
               << "Configured virtual starting balance: $"
               << number(result.settings.startingBalance) << " | fees: "
               << number(result.settings.feeBasisPoints, 1) << " bps | slippage: "
               << number(result.settings.slippageBasisPoints, 1) << " bps per fill\n"
               << "Reason: " << result.reason;
        return output.str();
    }
    output << "Closed simulated trades: " << result.trades.size() << '\n';
    if (result.trades.empty()) {
        output << "Win rate: N/A (no closed trades)\n"
               << "Average realized R: N/A (no closed trades)\n";
    } else {
        output << "Win rate: " << number(result.winRate.value_or(0.0), 1) << "% ("
               << result.wins << " wins / " << result.losses << " non-winning trades / "
               << result.trades.size() << " total)\n"
               << "Average realized risk/reward (net R): "
               << number(result.averageRiskReward.value_or(0.0), 2) << "R\n";
    }
    output << "Virtual starting balance: $" << number(result.settings.startingBalance) << '\n'
           << "Virtual ending balance: $" << number(result.endingBalance) << '\n'
           << "Simulated net P&L: " << money(result.netPnl) << '\n'
           << "Simulated fees: $" << number(result.totalFees) << '\n'
           << "Maximum drawdown: $" << number(result.maximumDrawdown) << " ("
           << number(result.maximumDrawdownPercent, 2) << "%)\n"
           << "Fees: " << number(result.settings.feeBasisPoints, 1)
           << " bps per fill | slippage: " << number(result.settings.slippageBasisPoints, 1)
           << " bps per fill | minimum TP1 risk/reward: 1:" << number(result.settings.minimumRiskReward, 2)
           << "\n"
           << "Sample size is limited to the supplied historical candles; results are simulated, not predictive.";
    return output.str();
}

std::string formatBacktestPerformanceMessage(std::span<const BacktestResult> results) {
    std::ostringstream output;
    output << "HISTORICAL BACKTEST PERFORMANCE — SIMULATED RESULTS; VIRTUAL BALANCES ONLY\n\n";
    if (results.empty()) {
        output << "No market backtest results are available.";
        return output.str();
    }
    for (std::size_t index = 0; index < results.size(); ++index) {
        if (index != 0) {
            output << "\n\n";
        }
        output << formatBacktestPerformance(results[index]);
    }
    return output.str();
}

std::string formatBacktestHistory(std::span<const SimulatedTrade> trades,
                                  std::size_t totalCount) {
    std::ostringstream output;
    output << "SIMULATED BACKTEST TRADE HISTORY — VIRTUAL ONLY\n";
    if (trades.empty()) {
        output << "No completed historical backtest trades are recorded yet.";
        return output.str();
    }
    output << "Showing " << trades.size() << " of " << totalCount << " recorded trades.\n";
    for (std::size_t index = 0; index < trades.size(); ++index) {
        const auto& trade = trades[index];
        output << "\n" << (index + 1) << ". " << simulatedDirectionName(trade.direction)
               << " " << marketSymbolName(trade.market) << " | " << trade.timeframe << '\n'
               << "Entry: " << formatUtc(trade.entryTime) << " @ $" << number(trade.entryPrice) << '\n'
               << "Exit: " << formatUtc(trade.exitTime) << " @ $" << number(trade.averageExitPrice)
               << " (" << trade.exitReason << ")\n"
               << "Illustrative levels: stop $" << number(trade.stopLoss) << " | TP1 $"
               << number(trade.target1) << " | TP2 $" << number(trade.target2) << '\n'
               << "Quantity: " << number(trade.quantity, 6) << " | Net P&L: " << money(trade.netPnl)
               << " | Fees: $" << number(trade.feesPaid) << " | Realized R: "
               << number(trade.realizedRiskReward, 2) << "\n"
               << "Historical source: " << trade.source;
    }
    return output.str();
}

} // namespace trading_bot
