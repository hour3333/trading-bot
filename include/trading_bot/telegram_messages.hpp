#pragma once

#include "trading_bot/database.hpp"

#include <string>
#include <string_view>

namespace trading_bot {

[[nodiscard]] std::string formatWelcomeMessage();
[[nodiscard]] std::string formatHelpMessage();
[[nodiscard]] std::string formatUnavailableMessage(std::string_view command);
[[nodiscard]] std::string formatStatusMessage();
[[nodiscard]] std::string formatPaperSignalHistory(
    const std::vector<SignalHistoryEntry>& entries, std::size_t totalCount,
    std::string_view timezone = "Asia/Phnom_Penh");

} // namespace trading_bot
