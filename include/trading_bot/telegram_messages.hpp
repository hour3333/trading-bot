#pragma once

#include <string>
#include <string_view>

namespace trading_bot {

[[nodiscard]] std::string formatWelcomeMessage();
[[nodiscard]] std::string formatHelpMessage();
[[nodiscard]] std::string formatUnavailableMessage(std::string_view command);
[[nodiscard]] std::string formatStatusMessage();

} // namespace trading_bot
