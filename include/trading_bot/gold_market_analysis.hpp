#pragma once

#include "trading_bot/financial_news.hpp"
#include "trading_bot/signal_engine.hpp"
#include "trading_bot/technical_analysis.hpp"

#include <chrono>
#include <string>
#include <string_view>

namespace trading_bot {

struct GoldMarketAnalysisReport {
    std::chrono::system_clock::time_point analyzedAt{};
    TechnicalAnalysisResult technical;
    SignalScenario scenario;
    NewsSnapshot news;
    std::string status;
    std::string reason;
    bool freshCompletedCandles{false};
    bool expectedWeeklyClosure{false};
};

[[nodiscard]] bool isGoldWeeklyClosureExpected(
    std::chrono::system_clock::time_point timestamp);

[[nodiscard]] GoldMarketAnalysisReport calculateGoldM5Analysis(
    const MarketDataSnapshot& market, NewsSnapshot news,
    std::chrono::system_clock::time_point asOf = std::chrono::system_clock::now(),
    double minimumRiskReward = 1.5);

[[nodiscard]] std::string formatGoldM5AnalysisMessage(
    const GoldMarketAnalysisReport& report,
    std::string_view geminiAssessment = {},
    std::string_view timezone = "Asia/Phnom_Penh");

// Produces a stable semantic key for change notifications. Candle timestamps
// and small indicator value changes do not create alerts by themselves.
[[nodiscard]] std::string goldAnalysisChangeKey(
    const GoldMarketAnalysisReport& report,
    std::string_view importantNewsKey = {});

} // namespace trading_bot
