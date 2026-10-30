#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trading_bot {

enum class MarketSymbol { XauUsd, BtcUsd };

[[nodiscard]] std::string marketSymbolName(MarketSymbol symbol);
[[nodiscard]] std::string marketDisplayName(MarketSymbol symbol);

struct MarketQuote {
    std::string symbol;
    std::string source;
    double price{0.0};
    std::chrono::system_clock::time_point providerTimestamp;
    std::chrono::system_clock::time_point fetchedAt;
    bool delayed{false};
};

struct OhlcCandle {
    std::string symbol;
    std::string source;
    // Provider's native interval. Gold API is a requested timestamp range,
    // while CoinGecko Demo currently auto-selects a fixed candle interval.
    std::string interval;
    std::chrono::system_clock::time_point startTime;
    std::chrono::system_clock::time_point endTime;
    double open{0.0};
    double high{0.0};
    double low{0.0};
    double close{0.0};
    std::chrono::system_clock::time_point fetchedAt;
    bool delayed{false};
};

struct MarketDataSnapshot {
    MarketSymbol market{MarketSymbol::XauUsd};
    std::optional<MarketQuote> quote;
    std::vector<OhlcCandle> candles;
    std::string historyDescription;
};

class MarketDataService {
public:
    virtual ~MarketDataService() = default;
    [[nodiscard]] virtual MarketDataSnapshot getSnapshot(MarketSymbol symbol) = 0;

    // Returns genuine provider candles at the requested fixed interval. The
    // default implementation may aggregate complete, contiguous lower
    // timeframe candles (for example 30m -> H1/H4); it never interpolates or
    // manufactures candles from a spot quote.
    [[nodiscard]] virtual MarketDataSnapshot getSnapshot(
        MarketSymbol symbol, std::string_view timeframe);
};

// Applies a fixed signal timeframe to a provider snapshot. Unsupported or
// incomplete interval conversions return an empty candle set.
[[nodiscard]] MarketDataSnapshot filterMarketSnapshotToTimeframe(
    MarketDataSnapshot snapshot, std::string_view timeframe);

} // namespace trading_bot
