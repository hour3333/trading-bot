#pragma once

#include "trading_bot/error.hpp"
#include "trading_bot/http_client.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trading_bot {

class TelegramApiError final : public AppError {
public:
    TelegramApiError(std::string method, long httpStatus, int apiErrorCode,
                     std::optional<long> retryAfterSeconds);

    [[nodiscard]] long httpStatus() const noexcept { return httpStatus_; }
    [[nodiscard]] int apiErrorCode() const noexcept { return apiErrorCode_; }
    [[nodiscard]] std::optional<long> retryAfterSeconds() const noexcept { return retryAfterSeconds_; }
    [[nodiscard]] bool retryable() const noexcept;

private:
    long httpStatus_;
    int apiErrorCode_;
    std::optional<long> retryAfterSeconds_;
};

struct TelegramMessage {
    std::optional<std::int64_t> userId;
    std::optional<std::int64_t> chatId;
    bool privateChat{false};
    std::string text;
};

struct TelegramUpdate {
    std::int64_t updateId{0};
    std::optional<TelegramMessage> message;
};

struct TelegramBotIdentity {
    std::int64_t id{0};
    std::string username;
};

class TelegramApiClient {
public:
    TelegramApiClient(std::string botToken, const HttpClientInterface& httpClient);

    [[nodiscard]] std::vector<TelegramUpdate> getUpdates(
        std::int64_t offset, long timeoutSeconds) const;
    [[nodiscard]] TelegramBotIdentity getMe() const;
    void sendMessage(std::int64_t chatId, const std::string& text) const;

private:
    [[nodiscard]] nlohmann::json call(std::string_view method,
                                      const nlohmann::json& parameters) const;

    std::string botToken_;
    const HttpClientInterface* httpClient_;
};

} // namespace trading_bot
