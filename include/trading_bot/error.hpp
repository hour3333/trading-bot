#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace trading_bot {

enum class ErrorCode {
    Configuration,
    Network,
    Json,
    Storage,
    Telegram,
    Internal,
};

[[nodiscard]] constexpr std::string_view errorCodeName(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Configuration:
        return "configuration";
    case ErrorCode::Network:
        return "network";
    case ErrorCode::Json:
        return "json";
    case ErrorCode::Storage:
        return "storage";
    case ErrorCode::Telegram:
        return "telegram";
    case ErrorCode::Internal:
        return "internal";
    }
    return "unknown";
}

class AppError : public std::runtime_error {
public:
    AppError(ErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

} // namespace trading_bot
