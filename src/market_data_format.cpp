#include "trading_bot/market_data_format.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace trading_bot {
namespace {

std::string displayInterval(const std::string& interval) {
    if (interval == "30m") {
        return "30-minute provider candle";
    }
    if (interval == "4h") {
        return "4-hour provider candle";
    }
    if (interval == "4d") {
        return "4-day provider candle";
    }
    return interval;
}

std::string formatUtc(std::chrono::system_clock::time_point timestamp) {
    const auto time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%d %H:%M:%S UTC");
    return output.str();
}

} // namespace

std::string formatMarketDataMessage(const MarketDataSnapshot& snapshot) {
    std::ostringstream output;
    output << marketDisplayName(snapshot.market) << " (" << marketSymbolName(snapshot.market) << ")\n";
    if (snapshot.quote) {
        output << "Latest reported price: $" << std::fixed << std::setprecision(2)
               << snapshot.quote->price << '\n'
               << "Provider: " << snapshot.quote->source << '\n'
               << "Provider timestamp: " << formatUtc(snapshot.quote->providerTimestamp) << '\n';
        if (snapshot.quote->delayed) {
            output << "DATA DELAYED: provider timestamp is older than expected.\n";
        }
    } else {
        output << "Price: DATA UNAVAILABLE\n";
    }

    if (snapshot.candles.empty()) {
        output << snapshot.historyDescription << ": DATA UNAVAILABLE";
    } else {
        const auto& candle = snapshot.candles.back();
        output << snapshot.historyDescription << " (" << displayInterval(candle.interval) << ")\n"
               << "Period: " << formatUtc(candle.startTime) << " to "
               << formatUtc(candle.endTime) << '\n'
               << "OHLC: $" << std::fixed << std::setprecision(2)
               << candle.open << " / $" << candle.high << " / $" << candle.low
               << " / $" << candle.close << '\n'
               << "History source: " << candle.source;
        if (candle.delayed) {
            output << "\nDATA DELAYED: latest provider candle is older than expected.";
        }
    }
    return output.str();
}

} // namespace trading_bot
