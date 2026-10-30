#include "trading_bot/telegram_messages.hpp"

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
           "/signal — Educational paper-trading scenarios\n"
           "/history — Recent simulated backtest trades\n"
           "/performance — Historical backtest performance\n"
           "/news — Financial headlines and AI explanation\n"
           "/calendar — Upcoming BLS economic releases\n"
           "/analysis — Technical indicators and AI context\n"
           "/status — Bot status";
}

std::string formatUnavailableMessage(std::string_view command) {
    return "Feature not available yet: " + std::string(command) + ".";
}

std::string formatStatusMessage() {
    return "Bot is online. Trading is disabled; market data is available when provider configuration allows it.";
}

} // namespace trading_bot
