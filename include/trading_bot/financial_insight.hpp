#pragma once

#include "trading_bot/financial_news.hpp"
#include "trading_bot/gemini_client.hpp"
#include "trading_bot/signal_engine.hpp"
#include "trading_bot/technical_analysis.hpp"

#include <chrono>
#include <optional>
#include <span>
#include <string>

namespace trading_bot {

class Database;

enum class ExplanationStatus {
    Available,
    NotConfigured,
    NoEvidence,
    InvalidCredentials,
    QuotaExhausted,
    RateLimited,
    InvalidResponse,
    ServiceUnavailable,
};

struct AiExplanation {
    ExplanationStatus status{ExplanationStatus::NotConfigured};
    std::string text;
    std::chrono::system_clock::time_point generatedAt{};
    bool fromCache{false};
};

class FinancialInsightService {
public:
    FinancialInsightService(Database& database, FinancialNewsService& newsService,
                            GeminiClient* geminiClient);

    [[nodiscard]] NewsSnapshot getLatestNews(std::size_t limit = 6);
    [[nodiscard]] AiExplanation explainNews(const NewsSnapshot& news);
    [[nodiscard]] AiExplanation explainSignal(const SignalScenario& scenario);
    [[nodiscard]] AiExplanation analyzeMarkets(
        std::span<const TechnicalAnalysisResult> analyses, const NewsSnapshot& news);

private:
    [[nodiscard]] AiExplanation generateCached(std::string_view task,
                                               std::string_view userData);

    Database* database_;
    FinancialNewsService* newsService_;
    GeminiClient* geminiClient_;
};

[[nodiscard]] std::string formatAiExplanation(const AiExplanation& explanation);
[[nodiscard]] std::string formatMarketAnalysisMessage(
    std::span<const TechnicalAnalysisResult> analyses, const NewsSnapshot& news,
    const AiExplanation& explanation);

} // namespace trading_bot
