#include "trading_bot/integration_verifier.hpp"

#include "trading_bot/database.hpp"
#include "trading_bot/economic_calendar.hpp"
#include "trading_bot/error.hpp"
#include "trading_bot/financial_news.hpp"
#include "trading_bot/gemini_client.hpp"
#include "trading_bot/json_utils.hpp"
#include "trading_bot/market_data.hpp"
#include "trading_bot/provider_market_data_service.hpp"
#include "trading_bot/telegram_api.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>

namespace trading_bot {
namespace {

std::string formatUtc(std::chrono::system_clock::time_point timestamp) {
    const auto raw = std::chrono::system_clock::to_time_t(timestamp);
    std::tm utc{};
    gmtime_r(&raw, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string httpFailureStatus(long status) {
    if (status == 401 || status == 403) {
        return "CREDENTIAL_REJECTED";
    }
    if (status == 429) {
        return "RATE_LIMITED";
    }
    if (status >= 500 || status == 0) {
        return "SERVICE_UNAVAILABLE";
    }
    return "API_ERROR";
}

bool positiveNumber(const nlohmann::json& payload, std::string_view key) {
    const auto found = payload.find(std::string(key));
    return found != payload.end() && found->is_number() &&
           std::isfinite(found->get<double>()) && found->get<double>() > 0.0;
}

void checkGoldApi(const Config& config, const HttpClientInterface& http,
                  Database& database, std::vector<IntegrationCheck>& output) {
    ProviderMarketDataService spotService(database, http, std::nullopt, std::nullopt);
    const auto snapshot = spotService.getSnapshot(MarketSymbol::XauUsd);
    if (!snapshot.quote) {
        output.push_back({"Gold API spot", "DATA_UNAVAILABLE",
                          "The public XAU/USD response did not pass provider validation"});
        return;
    }
    std::ostringstream detail;
    detail << std::fixed << std::setprecision(2) << "XAU/USD price=$" << snapshot.quote->price
           << "; source=" << snapshot.quote->source
           << "; provider_updated_at=" << formatUtc(snapshot.quote->providerTimestamp)
           << "; API key not required for spot";
    output.push_back({"Gold API spot", snapshot.quote->delayed ? "DATA_DELAYED" : "OK", detail.str()});

    if (!config.goldApiKey) {
        output.push_back({"Gold API history key", "NOT_CONFIGURED",
                          "Spot works without a key; historical OHLC needs GOLD_API_KEY"});
        return;
    }
    const auto end = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto start = end - std::chrono::duration_cast<std::chrono::seconds>(std::chrono::hours(24)).count();
    const auto history = http.request(HttpRequest{
        "GET", "https://api.gold-api.com/ohlc/XAU?startTimestamp=" + std::to_string(start) +
                   "&endTimestamp=" + std::to_string(end),
        {"Accept: application/json", "x-api-key: " + *config.goldApiKey}, {},
    });
    if (!history.isSuccess()) {
        output.push_back({"Gold API history key", httpFailureStatus(history.statusCode),
                          "Free OHLC endpoint returned HTTP " + std::to_string(history.statusCode) +
                              "; this check used one history/OHLC request"});
        return;
    }
    const auto ohlc = parseJson(history.body);
    const bool validOhlc = ohlc.is_object() && positiveNumber(ohlc, "open") &&
                           positiveNumber(ohlc, "high") && positiveNumber(ohlc, "low") &&
                           positiveNumber(ohlc, "close") && ohlc.contains("startTimestamp") &&
                           ohlc.contains("endTimestamp");
    output.push_back({"Gold API history key", validOhlc ? "OK" : "INVALID_RESPONSE",
                      validOhlc ? "GOLD_API_KEY accepted; validated provider OHLC response (one request)"
                                : "History response failed OHLC field validation"});
}

std::string sourceList(const std::vector<std::string>& sources) {
    if (sources.empty()) {
        return "none";
    }
    std::ostringstream output;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        output << sources[index];
    }
    return output.str();
}

void checkCoinGecko(const Config& config, Database& database,
                    const HttpClientInterface& http, std::vector<IntegrationCheck>& output) {
    if (!config.coingeckoDemoApiKey) {
        output.push_back({"CoinGecko Demo", "NOT_CONFIGURED",
                          "Set COINGECKO_DEMO_API_KEY to verify Demo API access and BTC data"});
        return;
    }
    const auto ping = http.request(HttpRequest{
        "GET", "https://api.coingecko.com/api/v3/ping",
        {"Accept: application/json", "x-cg-demo-api-key: " + *config.coingeckoDemoApiKey}, {},
    });
    if (!ping.isSuccess()) {
        output.push_back({"CoinGecko Demo", httpFailureStatus(ping.statusCode),
                          "Demo ping returned HTTP " + std::to_string(ping.statusCode)});
        return;
    }
    const auto pingPayload = parseJson(ping.body);
    if (!pingPayload.is_object()) {
        output.push_back({"CoinGecko Demo", "INVALID_RESPONSE", "Demo ping was not a JSON object"});
        return;
    }
    ProviderMarketDataService market(database, http, config.coingeckoDemoApiKey, std::nullopt);
    const auto snapshot = market.getSnapshot(MarketSymbol::BtcUsd);
    if (!snapshot.quote) {
        output.push_back({"CoinGecko Demo", "AUTH_OK_DATA_UNAVAILABLE",
                          "Ping accepted the Demo key but no validated BTC/USD quote was returned"});
        return;
    }
    std::ostringstream detail;
    detail << std::fixed << std::setprecision(2) << "BTC/USD price=$" << snapshot.quote->price
           << "; source=" << snapshot.quote->source
           << "; provider_timestamp=" << formatUtc(snapshot.quote->providerTimestamp)
           << "; OHLC candles=" << snapshot.candles.size();
    output.push_back({"CoinGecko Demo", snapshot.quote->delayed ? "DATA_DELAYED" : "OK", detail.str()});
}

} // namespace

std::vector<IntegrationCheck> verifyIntegrations(
    const Config& config, Database& temporaryDatabase,
    const HttpClientInterface& httpClient) {
    std::vector<IntegrationCheck> checks;

    if (!config.telegramBotToken) {
        checks.push_back({"Telegram Bot API", "NOT_CONFIGURED",
                          "Set TELEGRAM_BOT_TOKEN; no messages are sent by this check"});
    } else {
        try {
            TelegramApiClient telegram(*config.telegramBotToken, httpClient);
            (void)telegram.getMe();
            checks.push_back({"Telegram Bot API", "OK",
                              "getMe authenticated the token; no messages were sent"});
        } catch (const TelegramApiError& error) {
            checks.push_back({"Telegram Bot API",
                              error.httpStatus() == 401 || error.httpStatus() == 403
                                  ? "CREDENTIAL_REJECTED"
                                  : (error.httpStatus() == 429 ? "RATE_LIMITED" : "API_ERROR"),
                              "getMe failed with HTTP " + std::to_string(error.httpStatus()) +
                                  "; token omitted"});
        } catch (const std::exception&) {
            checks.push_back({"Telegram Bot API", "NETWORK_OR_RESPONSE_ERROR",
                              "getMe did not return a validated response; token omitted"});
        }
    }

    if (!config.geminiApiKey) {
        checks.push_back({"Gemini API", "NOT_CONFIGURED",
                          "Set GEMINI_API_KEY; no content generation is performed by this check"});
    } else {
        try {
            GeminiClient gemini(*config.geminiApiKey, httpClient);
            (void)gemini.verifyAccess();
            checks.push_back({"Gemini API", "OK",
                              "Model metadata accepted key for " + gemini.model() +
                                  "; no prompt, generation, grounding, or token charge requested"});
        } catch (const GeminiApiError& error) {
            std::string status = "API_ERROR";
            switch (error.failure()) {
            case GeminiFailure::InvalidCredentials: status = "CREDENTIAL_REJECTED"; break;
            case GeminiFailure::QuotaExhausted: status = "QUOTA_EXHAUSTED"; break;
            case GeminiFailure::RateLimited: status = "RATE_LIMITED"; break;
            case GeminiFailure::ServiceUnavailable: status = "SERVICE_UNAVAILABLE"; break;
            case GeminiFailure::InvalidRequest:
            case GeminiFailure::InvalidResponse: status = "INVALID_RESPONSE"; break;
            }
            checks.push_back({"Gemini API", std::move(status),
                              "Model metadata check failed; API key omitted"});
        } catch (const std::exception&) {
            checks.push_back({"Gemini API", "NETWORK_OR_RESPONSE_ERROR",
                              "Model metadata check failed; API key omitted"});
        }
    }

    try {
        checkCoinGecko(config, temporaryDatabase, httpClient, checks);
    } catch (const std::exception&) {
        checks.push_back({"CoinGecko Demo", "NETWORK_OR_RESPONSE_ERROR",
                          "Demo API response could not be validated; key omitted"});
    }
    try {
        checkGoldApi(config, httpClient, temporaryDatabase, checks);
    } catch (const std::exception&) {
        checks.push_back({"Gold API", "NETWORK_OR_RESPONSE_ERROR",
                          "Spot or history response could not be validated; API key omitted"});
    }

    try {
        RssFinancialNewsService news(temporaryDatabase, httpClient);
        const auto snapshot = news.getLatestNews(12);
        const auto totalFeeds = defaultFinancialNewsFeeds().size();
        const auto healthyFeeds = totalFeeds - std::min(totalFeeds, snapshot.unavailableSources.size());
        const std::string status = healthyFeeds == 0 ? "DATA_UNAVAILABLE"
            : (!snapshot.delayedSources.empty() || healthyFeeds < totalFeeds ? "PARTIAL" : "OK");
        checks.push_back({"Federal Reserve and BLS RSS", status,
                          "validated_articles=" + std::to_string(snapshot.articles.size()) +
                              "; feeds_ok=" + std::to_string(healthyFeeds) + "/" +
                              std::to_string(totalFeeds) + "; stale_feeds=" +
                              std::to_string(snapshot.delayedSources.size()) +
                              "; unavailable=" + sourceList(snapshot.unavailableSources) +
                              "; delayed=" + sourceList(snapshot.delayedSources)});
    } catch (const std::exception&) {
        checks.push_back({"Federal Reserve and BLS RSS", "NETWORK_OR_RESPONSE_ERROR",
                          "RSS responses could not be validated"});
    }

    try {
        BlsEconomicCalendarService calendar(temporaryDatabase, httpClient);
        const auto snapshot = calendar.getUpcomingEvents();
        checks.push_back({"BLS economic calendar", snapshot.unavailable ? "DATA_UNAVAILABLE"
                                                                          : (snapshot.delayed ? "DATA_DELAYED" : "OK"),
                          "upcoming_events=" + std::to_string(snapshot.events.size()) +
                              "; source=official BLS iCalendar" +
                              (snapshot.unavailable ? "; no validated fresh or cached schedule" :
                               (snapshot.delayed ? "; cached schedule is delayed" : "")) +
                              "; schedule_timestamp=" + formatUtc(snapshot.fetchedAt)});
    } catch (const std::exception&) {
        checks.push_back({"BLS economic calendar", "NETWORK_OR_RESPONSE_ERROR",
                          "Official iCalendar response could not be validated"});
    }
    return checks;
}

std::string formatIntegrationReport(const std::vector<IntegrationCheck>& checks) {
    std::ostringstream output;
    output << "External integration verification (read-only; secrets omitted)\n"
           << "Gemini uses model metadata only; Telegram getMe sends no chat messages.\n";
    for (const auto& check : checks) {
        output << check.service << ": " << check.status;
        if (!check.detail.empty()) {
            output << " — " << check.detail;
        }
        output << '\n';
    }
    return output.str();
}

} // namespace trading_bot
