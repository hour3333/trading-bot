#pragma once

#include "trading_bot/market_data.hpp"

#include <string>

namespace trading_bot {

[[nodiscard]] std::string formatMarketDataMessage(const MarketDataSnapshot& snapshot);

} // namespace trading_bot
