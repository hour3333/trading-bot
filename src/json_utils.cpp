#include "trading_bot/json_utils.hpp"

#include "trading_bot/error.hpp"

namespace trading_bot {

nlohmann::json parseJson(std::string_view input) {
    try {
        return nlohmann::json::parse(input.begin(), input.end());
    } catch (const nlohmann::json::parse_error& error) {
        throw AppError(ErrorCode::Json, std::string("Invalid JSON: ") + error.what());
    }
}

std::string prettyJson(const nlohmann::json& value, int indent) {
    if (indent < 0) {
        throw AppError(ErrorCode::Json, "JSON indentation must not be negative");
    }
    try {
        return value.dump(indent);
    } catch (const nlohmann::json::exception& error) {
        throw AppError(ErrorCode::Json, std::string("Unable to serialize JSON: ") + error.what());
    }
}

} // namespace trading_bot
