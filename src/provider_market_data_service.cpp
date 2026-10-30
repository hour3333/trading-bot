#include "trading_bot/provider_market_data_service.hpp"

#include "trading_bot/database.hpp"
#include "trading_bot/error.hpp"
#include "trading_bot/json_utils.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

namespace trading_bot {
namespace {

using Clock = std::chrono::system_clock;
using Seconds = std::chrono::seconds;
using Milliseconds = std::chrono::milliseconds;

constexpr std::string_view kGoldSpotCacheKey = "gold-api:xau-usd:spot";
constexpr std::string_view kGoldOhlcCacheKey = "gold-api:xau:ohlc:range-24h";
constexpr std::string_view kBitcoinSpotCacheKey = "coingecko:bitcoin-usd:spot";
constexpr std::string_view kBitcoinOhlcCacheKey = "coingecko:bitcoin:ohlc:days-1";
constexpr std::string_view kBitcoinH4CacheKey = "coingecko:bitcoin:ohlc:days-30";
constexpr std::string_view kGoldSource = "Gold API";
constexpr std::string_view kBitcoinSource = "CoinGecko Demo API";

Clock::time_point fromEpochSeconds(std::int64_t value) {
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(Seconds(value)));
}

Clock::time_point fromEpochMilliseconds(std::int64_t value) {
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(Milliseconds(value)));
}

std::int64_t toEpochSeconds(Clock::time_point value) {
    return std::chrono::duration_cast<Seconds>(value.time_since_epoch()).count();
}

std::int64_t integerField(const nlohmann::json& value, std::string_view field) {
    const auto found = value.find(std::string(field));
    if (found == value.end() || !found->is_number_integer()) {
        throw AppError(ErrorCode::Json, "Market provider response has an invalid timestamp");
    }
    try {
        return found->get<std::int64_t>();
    } catch (const nlohmann::json::exception&) {
        throw AppError(ErrorCode::Json, "Market provider timestamp is outside the supported range");
    }
}

double numberField(const nlohmann::json& value, std::string_view field) {
    const auto found = value.find(std::string(field));
    if (found == value.end() || !found->is_number()) {
        throw AppError(ErrorCode::Json, "Market provider response is missing a numeric field");
    }
    const auto number = found->get<double>();
    if (!std::isfinite(number)) {
        throw AppError(ErrorCode::Json, "Market provider returned a non-finite number");
    }
    return number;
}

int digits(std::string_view value, std::size_t offset, std::size_t count) {
    if (offset + count > value.size()) {
        throw AppError(ErrorCode::Json, "Market provider returned an invalid UTC timestamp");
    }
    int result = 0;
    const auto* first = value.data() + offset;
    const auto* last = first + count;
    const auto [position, error] = std::from_chars(first, last, result);
    if (error != std::errc{} || position != last) {
        throw AppError(ErrorCode::Json, "Market provider returned an invalid UTC timestamp");
    }
    return result;
}

Clock::time_point parseIsoUtc(std::string_view value) {
    // Gold API documents updatedAt in ISO 8601 UTC; accept its second-resolution
    // form (YYYY-MM-DDTHH:MM:SSZ) and an optional fractional part.
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':') {
        throw AppError(ErrorCode::Json, "Gold API returned an invalid updatedAt timestamp");
    }
    const std::chrono::year_month_day date{
        std::chrono::year(digits(value, 0, 4)),
        std::chrono::month(static_cast<unsigned>(digits(value, 5, 2))),
        std::chrono::day(static_cast<unsigned>(digits(value, 8, 2))),
    };
    const int hour = digits(value, 11, 2);
    const int minute = digits(value, 14, 2);
    const int second = digits(value, 17, 2);
    if (!date.ok() || hour > 23 || minute > 59 || second > 60) {
        throw AppError(ErrorCode::Json, "Gold API returned an invalid updatedAt timestamp");
    }
    auto result = Clock::time_point(
        std::chrono::duration_cast<Clock::duration>(
            std::chrono::sys_days(date).time_since_epoch() + std::chrono::hours(hour) +
            std::chrono::minutes(minute) + std::chrono::seconds(second)));

    if (value.size() > 20 && value[19] == '.') {
        std::size_t position = 20;
        std::int64_t fraction = 0;
        int digitsRead = 0;
        while (position < value.size() && value[position] >= '0' && value[position] <= '9') {
            if (digitsRead < 3) {
                fraction = fraction * 10 + (value[position] - '0');
            }
            ++digitsRead;
            ++position;
        }
        if (digitsRead == 0 || position + 1 != value.size() || value[position] != 'Z') {
            throw AppError(ErrorCode::Json, "Gold API returned an invalid updatedAt timestamp");
        }
        while (digitsRead < 3) {
            fraction *= 10;
            ++digitsRead;
        }
        result += Milliseconds(fraction);
    } else if (value.size() != 20 || value[19] != 'Z') {
        throw AppError(ErrorCode::Json, "Gold API returned an invalid updatedAt timestamp");
    }
    return result;
}

void validateTimestamp(Clock::time_point timestamp, Clock::time_point now,
                       std::chrono::hours maximumAge) {
    if (timestamp.time_since_epoch() <= Clock::duration::zero() ||
        timestamp > now + std::chrono::minutes(5) || timestamp < now - maximumAge) {
        throw AppError(ErrorCode::Json, "Market provider returned an unavailable or invalid timestamp");
    }
}

void validatePrices(double open, double high, double low, double close) {
    if (open <= 0.0 || high <= 0.0 || low <= 0.0 || close <= 0.0 ||
        high < std::max(open, close) || high < low || low > std::min(open, close)) {
        throw AppError(ErrorCode::Json, "Market provider returned invalid OHLC values");
    }
}

bool cacheIsFresh(const ProviderCacheEntry& entry, std::chrono::seconds ttl,
                  Clock::time_point now) {
    const auto age = now - entry.fetchedAt;
    return age >= Clock::duration::zero() && age <= ttl;
}

HttpResponse get(const HttpClientInterface& http, const std::string& url,
                 std::vector<std::string> headers = {}) {
    const auto response = http.request(HttpRequest{"GET", url, std::move(headers), {}});
    if (!response.isSuccess()) {
        throw AppError(ErrorCode::Network,
                       "Market data provider returned HTTP " + std::to_string(response.statusCode));
    }
    return response;
}

MarketQuote parseGoldQuote(const nlohmann::json& payload,
                           Clock::time_point fetchedAt, bool forceDelayed) {
    if (!payload.is_object() || payload.value("symbol", "") != "XAU" ||
        payload.value("currency", "") != "USD") {
        throw AppError(ErrorCode::Json, "Gold API returned a response for the wrong market");
    }
    const double price = numberField(payload, "price");
    const auto time = parseIsoUtc(payload.value("updatedAt", std::string{}));
    const auto now = Clock::now();
    validateTimestamp(time, now, std::chrono::hours(24));
    if (price <= 0.0) {
        throw AppError(ErrorCode::Json, "Gold API returned an invalid price");
    }
    return MarketQuote{
        "XAU/USD", std::string(kGoldSource), price, time, fetchedAt,
        forceDelayed || now - time > std::chrono::minutes(5),
    };
}

MarketQuote parseBitcoinQuote(const nlohmann::json& payload,
                              Clock::time_point fetchedAt, bool forceDelayed) {
    const auto coin = payload.find("bitcoin");
    if (!payload.is_object() || coin == payload.end() || !coin->is_object()) {
        throw AppError(ErrorCode::Json, "CoinGecko response has no bitcoin price");
    }
    const double price = numberField(*coin, "usd");
    const auto time = fromEpochSeconds(integerField(*coin, "last_updated_at"));
    const auto now = Clock::now();
    validateTimestamp(time, now, std::chrono::hours(24));
    if (price <= 0.0) {
        throw AppError(ErrorCode::Json, "CoinGecko returned an invalid price");
    }
    return MarketQuote{
        "BTC/USD", std::string(kBitcoinSource), price, time, fetchedAt,
        forceDelayed || now - time > std::chrono::minutes(5),
    };
}

OhlcCandle parseGoldCandle(const nlohmann::json& payload,
                           Clock::time_point fetchedAt) {
    if (!payload.is_object()) {
        throw AppError(ErrorCode::Json, "Gold API OHLC response is not an object");
    }
    const double open = numberField(payload, "open");
    const double high = numberField(payload, "high");
    const double low = numberField(payload, "low");
    const double close = numberField(payload, "close");
    const auto start = fromEpochSeconds(integerField(payload, "startTimestamp"));
    const auto end = fromEpochSeconds(integerField(payload, "endTimestamp"));
    const auto now = Clock::now();
    if (start >= end || end > now + std::chrono::minutes(5) || start < now - std::chrono::hours(26)) {
        throw AppError(ErrorCode::Json, "Gold API returned an invalid OHLC time range");
    }
    validatePrices(open, high, low, close);
    return OhlcCandle{
        "XAU/USD", std::string(kGoldSource), "requested 24h range",
        start, end, open, high, low, close, fetchedAt,
        now - end > std::chrono::hours(2),
    };
}

std::vector<OhlcCandle> parseBitcoinCandles(const nlohmann::json& payload,
                                            Clock::time_point fetchedAt, int days = 1) {
    if (!payload.is_array() || payload.empty()) {
        throw AppError(ErrorCode::Json, "CoinGecko OHLC response contains no candles");
    }
    const auto now = Clock::now();
    const bool fourHour = days >= 3;
    const auto duration = fourHour ? std::chrono::hours(4) : std::chrono::minutes(30);
    const auto maximumAge = std::chrono::hours((days + 1) * 24);
    std::vector<OhlcCandle> candles;
    candles.reserve(payload.size());
    for (const auto& row : payload) {
        if (!row.is_array() || row.size() != 5 || !row[0].is_number_integer()) {
            throw AppError(ErrorCode::Json, "CoinGecko returned an invalid OHLC candle");
        }
        std::int64_t closeTimestamp = 0;
        try {
            closeTimestamp = row[0].get<std::int64_t>();
        } catch (const nlohmann::json::exception&) {
            throw AppError(ErrorCode::Json, "CoinGecko returned an invalid candle timestamp");
        }
        const auto end = fromEpochMilliseconds(closeTimestamp);
        const auto start = end - duration;
        const double open = row[1].is_number() ? row[1].get<double>() : 0.0;
        const double high = row[2].is_number() ? row[2].get<double>() : 0.0;
        const double low = row[3].is_number() ? row[3].get<double>() : 0.0;
        const double close = row[4].is_number() ? row[4].get<double>() : 0.0;
        validateTimestamp(end, now, maximumAge);
        validatePrices(open, high, low, close);
        candles.push_back(OhlcCandle{
            "BTC/USD", std::string(kBitcoinSource), fourHour ? "4h" : "30m",
            start, end, open, high, low, close, fetchedAt,
            now - end > duration * 2,
        });
    }
    std::sort(candles.begin(), candles.end(), [](const OhlcCandle& left, const OhlcCandle& right) {
        return left.startTime < right.startTime;
    });
    return candles;
}

void markDelayed(std::vector<OhlcCandle>& candles) {
    for (auto& candle : candles) {
        candle.delayed = true;
    }
}

std::optional<MarketQuote> quoteFromCache(const std::optional<ProviderCacheEntry>& entry,
                                          bool gold, bool forceDelayed) {
    if (!entry) {
        return std::nullopt;
    }
    try {
        const auto payload = parseJson(entry->payload);
        if (gold) {
            return parseGoldQuote(payload, entry->fetchedAt, forceDelayed);
        }
        return parseBitcoinQuote(payload, entry->fetchedAt, forceDelayed);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

ProviderMarketDataService::ProviderMarketDataService(
    Database& database, const HttpClientInterface& httpClient,
    std::optional<std::string> coingeckoDemoApiKey,
    std::optional<std::string> goldApiKey)
    : database_(&database), httpClient_(&httpClient),
      coingeckoDemoApiKey_(std::move(coingeckoDemoApiKey)),
      goldApiKey_(std::move(goldApiKey)) {}

MarketDataSnapshot ProviderMarketDataService::getSnapshot(MarketSymbol symbol) {
    std::lock_guard lock(requestMutex_);
    MarketDataSnapshot snapshot;
    snapshot.market = symbol;
    if (symbol == MarketSymbol::XauUsd) {
        snapshot.historyDescription = "Historical OHLC";
        try {
            snapshot.quote = getGoldQuote();
        } catch (const std::exception&) {
            snapshot.quote.reset();
        }
        try {
            snapshot.candles = getGoldCandles();
        } catch (const std::exception&) {
            snapshot.candles.clear();
        }
    } else {
        snapshot.historyDescription = "Historical OHLC";
        try {
            snapshot.quote = getBitcoinQuote();
        } catch (const std::exception&) {
            snapshot.quote.reset();
        }
        try {
            snapshot.candles = getBitcoinCandles();
        } catch (const std::exception&) {
            snapshot.candles.clear();
        }
    }
    return snapshot;
}

MarketDataSnapshot ProviderMarketDataService::getSnapshot(
    MarketSymbol symbol, std::string_view timeframe) {
    MarketDataSnapshot snapshot;
    snapshot.market = symbol;
    snapshot.historyDescription = "Configured provider fixed-interval history";
    std::lock_guard lock(requestMutex_);
    if (symbol == MarketSymbol::XauUsd) {
        snapshot.historyDescription =
            "Gold API free OHLC is a requested-range aggregate; no fixed M5/M15/H1/H4 candles are available";
        try {
            snapshot.quote = getGoldQuote();
        } catch (const std::exception&) {
            snapshot.quote.reset();
        }
        return filterMarketSnapshotToTimeframe(std::move(snapshot), timeframe);
    }

    try {
        snapshot.quote = getBitcoinQuote();
    } catch (const std::exception&) {
        snapshot.quote.reset();
    }
    if (timeframe == "H4" || timeframe == "h4" || timeframe == "4h") {
        try {
            snapshot.candles = getBitcoinCandles(30);
        } catch (const std::exception&) {
            snapshot.candles.clear();
        }
        snapshot.historyDescription = "CoinGecko Demo OHLC, native 4h candles from 30-day history";
    } else if (timeframe == "H1" || timeframe == "h1" || timeframe == "1h") {
        try {
            snapshot.candles = getBitcoinCandles(1);
        } catch (const std::exception&) {
            snapshot.candles.clear();
        }
        snapshot.historyDescription = "CoinGecko Demo OHLC 30m candles; H1 requires complete 2-candle aggregation";
    } else {
        snapshot.historyDescription =
            "CoinGecko Demo OHLC does not provide M5/M15 fixed candles on the configured free endpoint";
    }
    return filterMarketSnapshotToTimeframe(std::move(snapshot), timeframe);
}

std::optional<MarketQuote> ProviderMarketDataService::getGoldQuote() {
    constexpr auto ttl = std::chrono::seconds(30);
    const auto cache = database_->loadProviderCache(kGoldSpotCacheKey);
    const auto now = Clock::now();
    if (cache && cacheIsFresh(*cache, ttl, now)) {
        if (auto quote = quoteFromCache(cache, true, false)) {
            return quote;
        }
    }
    try {
        const auto response = get(*httpClient_, "https://api.gold-api.com/price/XAU/USD");
        const auto fetchedAt = Clock::now();
        const auto payload = parseJson(response.body);
        auto quote = parseGoldQuote(payload, fetchedAt, false);
        database_->saveProviderCache(kGoldSpotCacheKey, response.body, fetchedAt);
        return quote;
    } catch (const std::exception&) {
        return quoteFromCache(cache, true, true);
    }
}

std::optional<MarketQuote> ProviderMarketDataService::getBitcoinQuote() {
    constexpr auto ttl = std::chrono::seconds(60);
    const auto cache = database_->loadProviderCache(kBitcoinSpotCacheKey);
    const auto now = Clock::now();
    if (cache && cacheIsFresh(*cache, ttl, now)) {
        if (auto quote = quoteFromCache(cache, false, false)) {
            return quote;
        }
    }
    if (!coingeckoDemoApiKey_ || coingeckoDemoApiKey_->empty()) {
        return quoteFromCache(cache, false, true);
    }
    try {
        const auto response = get(*httpClient_,
            "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd&include_last_updated_at=true",
            {"x-cg-demo-api-key: " + *coingeckoDemoApiKey_});
        const auto fetchedAt = Clock::now();
        const auto payload = parseJson(response.body);
        auto quote = parseBitcoinQuote(payload, fetchedAt, false);
        database_->saveProviderCache(kBitcoinSpotCacheKey, response.body, fetchedAt);
        return quote;
    } catch (const std::exception&) {
        return quoteFromCache(cache, false, true);
    }
}

std::vector<OhlcCandle> ProviderMarketDataService::getGoldCandles() {
    constexpr auto ttl = std::chrono::hours(1);
    const auto cache = database_->loadProviderCache(kGoldOhlcCacheKey);
    const auto now = Clock::now();
    if (cache && cacheIsFresh(*cache, ttl, now)) {
        try {
            auto candles = std::vector<OhlcCandle>{parseGoldCandle(parseJson(cache->payload), cache->fetchedAt)};
            database_->replaceMarketCandles(kGoldOhlcCacheKey, candles);
            return candles;
        } catch (const std::exception&) {
            // Fetch a replacement if the cached provider response is malformed.
        }
    }
    if (goldApiKey_ && !goldApiKey_->empty()) {
        try {
            const auto end = toEpochSeconds(now);
            const auto start = end - std::chrono::duration_cast<Seconds>(std::chrono::hours(24)).count();
            const std::string url = "https://api.gold-api.com/ohlc/XAU?startTimestamp=" +
                                    std::to_string(start) + "&endTimestamp=" + std::to_string(end);
            const auto response = get(*httpClient_, url, {"x-api-key: " + *goldApiKey_});
            const auto fetchedAt = Clock::now();
            auto candles = std::vector<OhlcCandle>{parseGoldCandle(parseJson(response.body), fetchedAt)};
            database_->saveProviderCache(kGoldOhlcCacheKey, response.body, fetchedAt);
            database_->replaceMarketCandles(kGoldOhlcCacheKey, candles);
            return candles;
        } catch (const std::exception&) {
            // Fall back to validated persisted candles and mark them delayed.
        }
    }
    auto candles = database_->loadMarketCandles(kGoldOhlcCacheKey);
    markDelayed(candles);
    return candles;
}

std::vector<OhlcCandle> ProviderMarketDataService::getBitcoinCandles(int days) {
    if (days != 1 && days != 30) {
        return {};
    }
    const auto cacheKey = days == 30 ? kBitcoinH4CacheKey : kBitcoinOhlcCacheKey;
    const auto ttl = days == 30 ? std::chrono::duration_cast<std::chrono::seconds>(std::chrono::minutes(10))
                                : std::chrono::duration_cast<std::chrono::seconds>(std::chrono::minutes(5));
    const auto cache = database_->loadProviderCache(cacheKey);
    const auto now = Clock::now();
    if (cache && cacheIsFresh(*cache, ttl, now)) {
        try {
            auto candles = parseBitcoinCandles(parseJson(cache->payload), cache->fetchedAt, days);
            database_->replaceMarketCandles(cacheKey, candles);
            return candles;
        } catch (const std::exception&) {
            // Fetch a replacement if the cached provider response is malformed.
        }
    }
    if (coingeckoDemoApiKey_ && !coingeckoDemoApiKey_->empty()) {
        try {
            const auto response = get(*httpClient_,
                "https://api.coingecko.com/api/v3/coins/bitcoin/ohlc?vs_currency=usd&days=" +
                    std::to_string(days),
                {"x-cg-demo-api-key: " + *coingeckoDemoApiKey_});
            const auto fetchedAt = Clock::now();
            auto candles = parseBitcoinCandles(parseJson(response.body), fetchedAt, days);
            database_->saveProviderCache(cacheKey, response.body, fetchedAt);
            database_->replaceMarketCandles(cacheKey, candles);
            return candles;
        } catch (const std::exception&) {
            // Fall back to validated persisted candles and mark them delayed.
        }
    }
    auto candles = database_->loadMarketCandles(cacheKey);
    markDelayed(candles);
    return candles;
}

} // namespace trading_bot
