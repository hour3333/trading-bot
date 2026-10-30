#pragma once

#include "trading_bot/http_client.hpp"
#include "trading_bot/market_data.hpp"

#include <optional>
#include <mutex>
#include <string>
#include <string_view>

namespace trading_bot {

class Database;

// Free-tier provider implementation. CoinGecko Demo OHLC is requested at
// 30m (1 day) or 4h (30 days); Gold API's free OHLC endpoint is a requested
// range rather than a fixed signal interval, so gold signals stay WAIT.
class ProviderMarketDataService final : public MarketDataService {
public:
    ProviderMarketDataService(Database& database, const HttpClientInterface& httpClient,
                              std::optional<std::string> coingeckoDemoApiKey,
                              std::optional<std::string> goldApiKey);

    [[nodiscard]] MarketDataSnapshot getSnapshot(MarketSymbol symbol) override;
    [[nodiscard]] MarketDataSnapshot getSnapshot(
        MarketSymbol symbol, std::string_view timeframe) override;

private:
    [[nodiscard]] std::optional<MarketQuote> getGoldQuote();
    [[nodiscard]] std::optional<MarketQuote> getBitcoinQuote();
    [[nodiscard]] std::vector<OhlcCandle> getGoldCandles();
    [[nodiscard]] std::vector<OhlcCandle> getBitcoinCandles(int days = 1);

    Database* database_;
    const HttpClientInterface* httpClient_;
    std::optional<std::string> coingeckoDemoApiKey_;
    std::optional<std::string> goldApiKey_;
    std::mutex requestMutex_;
};

} // namespace trading_bot
