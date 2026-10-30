#include "trading_bot/market_data.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace trading_bot {
namespace {

struct FixedInterval {
    std::string canonical;
    std::chrono::seconds duration;
};

std::optional<FixedInterval> requestedInterval(std::string_view value) {
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    using namespace std::chrono;
    if (normalized == "M5") return FixedInterval{"5m", minutes(5)};
    if (normalized == "M15") return FixedInterval{"15m", minutes(15)};
    if (normalized == "H1") return FixedInterval{"1h", hours(1)};
    if (normalized == "H4") return FixedInterval{"4h", hours(4)};
    return std::nullopt;
}

std::optional<std::chrono::seconds> candleInterval(std::string_view value) {
    using namespace std::chrono;
    if (value == "5m") return minutes(5);
    if (value == "15m") return minutes(15);
    if (value == "30m") return minutes(30);
    if (value == "1h") return hours(1);
    if (value == "4h") return hours(4);
    return std::nullopt;
}

std::vector<OhlcCandle> aggregateCompleteCandles(
    const std::vector<OhlcCandle>& candles, const FixedInterval& target) {
    if (candles.empty()) return {};
    const auto sourceDuration = candleInterval(candles.front().interval);
    if (!sourceDuration || *sourceDuration >= target.duration ||
        target.duration.count() % sourceDuration->count() != 0) {
        return {};
    }
    // Aggregating minute bars is only allowed where every source candle is
    // present and exact. This permits 30m -> H1/H4 while refusing interpolation.
    const auto expected = static_cast<std::size_t>(target.duration.count() /
                                                    sourceDuration->count());
    if (expected < 2) return {};

    std::vector<OhlcCandle> result;
    for (std::size_t begin = 0; begin + expected <= candles.size();) {
        const auto& first = candles[begin];
        const auto startSeconds = std::chrono::duration_cast<std::chrono::seconds>(
            first.startTime.time_since_epoch()).count();
        if (first.startTime.time_since_epoch() <= std::chrono::system_clock::duration::zero() ||
            startSeconds % target.duration.count() != 0 ||
            first.endTime - first.startTime != *sourceDuration) {
            ++begin;
            continue;
        }

        bool complete = true;
        double high = first.high;
        double low = first.low;
        for (std::size_t offset = 0; offset < expected; ++offset) {
            const auto& candle = candles[begin + offset];
            const auto& prior = candles[begin + offset - (offset == 0 ? 0 : 1)];
            if (candle.symbol != first.symbol || candle.source != first.source ||
                candle.interval != first.interval || candle.endTime - candle.startTime != *sourceDuration ||
                (offset > 0 && candle.startTime != prior.endTime)) {
                complete = false;
                break;
            }
            high = std::max(high, candle.high);
            low = std::min(low, candle.low);
        }
        const auto& last = candles[begin + expected - 1];
        if (!complete || last.endTime - first.startTime != target.duration) {
            ++begin;
            continue;
        }

        auto fetchedAt = first.fetchedAt;
        bool delayed = false;
        for (std::size_t offset = 0; offset < expected; ++offset) {
            fetchedAt = std::max(fetchedAt, candles[begin + offset].fetchedAt);
            delayed = delayed || candles[begin + offset].delayed;
        }
        result.push_back(OhlcCandle{
            first.symbol,
            first.source + " (aggregated from " + first.interval + " candles)",
            target.canonical,
            first.startTime,
            last.endTime,
            first.open,
            high,
            low,
            last.close,
            fetchedAt,
            delayed,
        });
        begin += expected;
    }
    return result;
}

} // namespace

std::string marketSymbolName(MarketSymbol symbol) {
    switch (symbol) {
    case MarketSymbol::XauUsd:
        return "XAU/USD";
    case MarketSymbol::BtcUsd:
        return "BTC/USD";
    }
    return "UNKNOWN";
}

std::string marketDisplayName(MarketSymbol symbol) {
    switch (symbol) {
    case MarketSymbol::XauUsd:
        return "Gold";
    case MarketSymbol::BtcUsd:
        return "Bitcoin";
    }
    return "Unknown market";
}

MarketDataSnapshot MarketDataService::getSnapshot(MarketSymbol symbol,
                                                   std::string_view timeframe) {
    return filterMarketSnapshotToTimeframe(getSnapshot(symbol), timeframe);
}

MarketDataSnapshot filterMarketSnapshotToTimeframe(MarketDataSnapshot snapshot,
                                                    std::string_view timeframe) {
    const auto target = requestedInterval(timeframe);
    if (!target) {
        snapshot.candles.clear();
        snapshot.historyDescription = "Unsupported signal timeframe";
        return snapshot;
    }
    if (snapshot.candles.empty()) {
        if (snapshot.historyDescription.empty()) {
            snapshot.historyDescription = "No provider OHLC candles available for " + target->canonical;
        }
        return snapshot;
    }
    const bool exact = std::all_of(snapshot.candles.begin(), snapshot.candles.end(),
        [&](const OhlcCandle& candle) { return candle.interval == target->canonical; });
    if (!exact) {
        snapshot.candles = aggregateCompleteCandles(snapshot.candles, *target);
        snapshot.historyDescription = snapshot.candles.empty()
            ? "No complete provider candles available for " + target->canonical
            : "OHLC aggregated from complete provider candles for " + target->canonical;
    }
    return snapshot;
}

} // namespace trading_bot
