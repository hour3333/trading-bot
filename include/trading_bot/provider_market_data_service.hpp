#pragma once

#include "trading_bot/http_client.hpp"
#include "trading_bot/market_data.hpp"

#include <optional>
#include <mutex>
#include <string>

namespace trading_bot {

class Database;

// Free-tier provider implementation. Raw responses are cached in SQLite and
// parsed candles are stored in the normalized market_candles table.
class ProviderMarketDataService final : public MarketDataService {
public:
    ProviderMarketDataService(Database& database, const HttpClientInterface& httpClient,
                              std::optional<std::string> coingeckoDemoApiKey,
                              std::optional<std::string> goldApiKey);

    [[nodiscard]] MarketDataSnapshot getSnapshot(MarketSymbol symbol) override;

private:
    [[nodiscard]] std::optional<MarketQuote> getGoldQuote();
    [[nodiscard]] std::optional<MarketQuote> getBitcoinQuote();
    [[nodiscard]] std::vector<OhlcCandle> getGoldCandles();
    [[nodiscard]] std::vector<OhlcCandle> getBitcoinCandles();

    Database* database_;
    const HttpClientInterface* httpClient_;
    std::optional<std::string> coingeckoDemoApiKey_;
    std::optional<std::string> goldApiKey_;
    std::mutex requestMutex_;
};

} // namespace trading_bot
