#include "trading_bot/telegram_messages.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace trading_bot {

std::string formatWelcomeMessage() {
    return "Welcome. Use /help to see the available bot commands.";
}

std::string formatHelpMessage() {
    return "Available commands:\n"
           "/start — Start the bot\n"
           "/help — Show this help\n"
           "/gold — Gold market information\n"
           "/btc — Bitcoin market information\n"
           "/signal — Latest XAU/USD M5 confirmed paper scenario\n"
           "/m5 — XAU/USD M5 technical analysis\n"
           "/signal_m5 — M5 educational scenarios (only with real M5 candles)\n"
           "/signal_m15 — M15 educational scenarios (only with real M15 candles)\n"
           "/signal_h1 — H1 educational scenarios\n"
           "/signal_h4 — H4 educational scenarios\n"
           "/history — Saved paper signals and simulated backtest trades\n"
           "/performance — Historical backtest performance\n"
           "/news — Latest gold-relevant headlines, impact class, and optional AI explanation\n"
           "/news_on — Enable HIGH and MEDIUM gold news alerts\n"
           "/news_off — Disable your automatic gold news alerts\n"
           "/news_status — Show your gold news alert settings\n"
           "/news_high — Enable HIGH-impact alerts only\n"
           "/news_all — Enable HIGH- and MEDIUM-impact alerts\n"
           "/news_test — Send a labeled notification-path diagnostic\n"
           "/calendar — Upcoming BLS economic releases\n"
           "/analysis — Validated XAU/USD M5 analysis with sourced news and Gemini context\n"
           "/m5 — XAU/USD M5 indicators, only when genuine completed candles are available\n"
           "/analysis_h4 — Previous multi-market H4 analysis\n"
           "/analysis_on — Enable significant XAU/USD M5 analysis notifications\n"
           "/analysis_off — Disable your XAU/USD M5 analysis notifications\n"
           "/analysis_status — Show analysis update time and data freshness\n"
           "/status — Bot status";
}

std::string formatUnavailableMessage(std::string_view command) {
    return "Feature not available yet: " + std::string(command) + ".";
}

std::string formatStatusMessage() {
    return "Bot is online. Trading is disabled; market data is available when provider configuration allows it.";
}

std::string formatPaperSignalHistory(const std::vector<SignalHistoryEntry>& entries,
                                     std::size_t totalCount,
                                     std::string_view timezone) {
    std::ostringstream output;
    output << "PAPER SIGNAL HISTORY — EDUCATIONAL ONLY\n";
    if (entries.empty()) {
        output << "No saved paper-signal scenarios are recorded yet.";
        return output.str();
    }
    output << "Showing " << entries.size() << " of " << totalCount << " saved scenarios.\n";
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        const auto raw = std::chrono::system_clock::to_time_t(entry.analysisAt);
        std::tm local{};
        const bool converted = localtime_r(&raw, &local) != nullptr;
        output << '\n' << (index + 1) << ". " << marketSymbolName(entry.market)
               << " | " << entry.timeframe << " | " << entry.state << "\nDate/time: ";
        if (converted) output << std::put_time(&local, "%Y-%m-%d %H:%M") << " (" << timezone << ')';
        else output << "DATA UNAVAILABLE";
        output << "\nData: " << entry.dataStatus << " | Trend: " << entry.trend << '\n';
        if (entry.entryLow && entry.entryHigh && entry.stopLoss && entry.takeProfit1 && entry.takeProfit2) {
            output << "Entry zone: $" << std::fixed << std::setprecision(2) << *entry.entryLow
                   << "–$" << *entry.entryHigh << " | Stop: $" << *entry.stopLoss
                   << " | TP1: $" << *entry.takeProfit1 << " | TP2: $" << *entry.takeProfit2 << '\n';
            if (entry.riskReward1 && entry.riskReward2) {
                output << "Risk/reward: TP1 1:" << *entry.riskReward1
                       << ", TP2 1:" << *entry.riskReward2 << '\n';
            }
        } else {
            output << "Levels: DATA UNAVAILABLE\n";
        }
        output << "Source: " << (entry.dataSource.empty() ? "DATA UNAVAILABLE" : entry.dataSource);
        if (entry.candleTimestamp) {
            const auto candleRaw = std::chrono::system_clock::to_time_t(*entry.candleTimestamp);
            std::tm candleLocal{};
            output << " | Completed candle: ";
            if (localtime_r(&candleRaw, &candleLocal) != nullptr) {
                output << std::put_time(&candleLocal, "%Y-%m-%d %H:%M") << " (" << timezone << ')';
            } else output << "DATA UNAVAILABLE";
        }
        output << '\n';
    }
    return output.str();
}

} // namespace trading_bot
