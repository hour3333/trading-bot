#include "trading_bot/market_data.hpp"

namespace trading_bot {

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

} // namespace trading_bot
