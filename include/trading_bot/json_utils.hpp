#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace trading_bot {

[[nodiscard]] nlohmann::json parseJson(std::string_view input);
[[nodiscard]] std::string prettyJson(const nlohmann::json& value, int indent = 2);

} // namespace trading_bot
