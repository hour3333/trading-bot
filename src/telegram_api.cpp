#include "trading_bot/telegram_api.hpp"

#include "trading_bot/error.hpp"
#include "trading_bot/json_utils.hpp"

#include <algorithm>
#include <limits>
#include <string_view>

namespace trading_bot {
namespace {

std::optional<std::int64_t> optionalInteger(const nlohmann::json& object,
                                            std::string_view key) {
    const auto found = object.find(std::string(key));
    if (found == object.end() || found->is_null()) {
        return std::nullopt;
    }
    if (!found->is_number_integer()) {
        throw AppError(ErrorCode::Json, "Telegram response has an invalid integer field");
    }
    try {
        return found->get<std::int64_t>();
    } catch (const nlohmann::json::exception&) {
        throw AppError(ErrorCode::Json, "Telegram response integer is outside the supported range");
    }
}

std::optional<long> retryAfter(const nlohmann::json& payload) {
    const auto parameters = payload.find("parameters");
    if (parameters == payload.end() || !parameters->is_object()) {
        return std::nullopt;
    }
    const auto value = parameters->find("retry_after");
    if (value == parameters->end() || !value->is_number_integer()) {
        return std::nullopt;
    }
    try {
        const auto seconds = value->get<long>();
        return seconds > 0 ? std::optional<long>(seconds) : std::nullopt;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

TelegramMessage parseMessage(const nlohmann::json& value) {
    TelegramMessage message;
    if (!value.is_object()) {
        throw AppError(ErrorCode::Json, "Telegram update message is not an object");
    }

    const auto from = value.find("from");
    if (from != value.end() && from->is_object()) {
        message.userId = optionalInteger(*from, "id");
    }
    const auto chat = value.find("chat");
    if (chat != value.end() && chat->is_object()) {
        message.chatId = optionalInteger(*chat, "id");
        const auto type = chat->find("type");
        message.privateChat = type != chat->end() && type->is_string() &&
                              type->get<std::string>() == "private";
    }
    const auto text = value.find("text");
    if (text != value.end() && text->is_string()) {
        message.text = text->get<std::string>();
    }
    return message;
}

} // namespace

TelegramApiError::TelegramApiError(std::string method, long httpStatus, int apiErrorCode,
                                   std::optional<long> retryAfterSeconds)
    : AppError(ErrorCode::Telegram, "Telegram method " + method + " failed (HTTP " +
               std::to_string(httpStatus) + ", API " + std::to_string(apiErrorCode) + ")"),
      httpStatus_(httpStatus), apiErrorCode_(apiErrorCode),
      retryAfterSeconds_(retryAfterSeconds) {}

bool TelegramApiError::retryable() const noexcept {
    return httpStatus_ == 408 || httpStatus_ == 429 || httpStatus_ >= 500 ||
           apiErrorCode_ == 408 || apiErrorCode_ == 429 || apiErrorCode_ >= 500;
}

TelegramApiClient::TelegramApiClient(std::string botToken,
                                     const HttpClientInterface& httpClient)
    : botToken_(std::move(botToken)), httpClient_(&httpClient) {
    if (botToken_.empty()) {
        throw AppError(ErrorCode::Configuration, "TELEGRAM_BOT_TOKEN must be set to enable the Telegram bot");
    }
}

TelegramBotIdentity TelegramApiClient::getMe() const {
    const auto response = call("getMe", nlohmann::json::object());
    const auto result = response.find("result");
    if (result == response.end() || !result->is_object()) {
        throw AppError(ErrorCode::Json, "Telegram getMe response has no bot identity");
    }
    const auto id = optionalInteger(*result, "id");
    const auto isBot = result->find("is_bot");
    if (!id || *id <= 0 || isBot == result->end() || !isBot->is_boolean() || !isBot->get<bool>()) {
        throw AppError(ErrorCode::Json, "Telegram getMe response is not a valid bot identity");
    }
    std::string username;
    const auto name = result->find("username");
    if (name != result->end() && name->is_string()) {
        username = name->get<std::string>();
    }
    return {*id, std::move(username)};
}

nlohmann::json TelegramApiClient::call(std::string_view method,
                                       const nlohmann::json& parameters) const {
    const std::string methodName(method);
    const std::string url = "https://api.telegram.org/bot" + botToken_ + "/" + methodName;
    const HttpRequest request{
        "POST",
        url,
        {"Content-Type: application/json", "Accept: application/json"},
        parameters.dump(),
    };
    const auto response = httpClient_->request(request);
    const auto payload = parseJson(response.body);
    if (!payload.is_object() || !payload.contains("ok") || !payload.at("ok").is_boolean()) {
        throw AppError(ErrorCode::Json, "Telegram response has an invalid API envelope");
    }

    if (!response.isSuccess() || !payload.at("ok").get<bool>()) {
        int apiCode = response.statusCode > 0 && response.statusCode <= std::numeric_limits<int>::max()
                          ? static_cast<int>(response.statusCode)
                          : 0;
        if (const auto code = payload.find("error_code");
            code != payload.end() && code->is_number_integer()) {
            try {
                apiCode = code->get<int>();
            } catch (const nlohmann::json::exception&) {
                // Keep the HTTP status as the safe fallback for malformed error codes.
            }
        }
        throw TelegramApiError(methodName, response.statusCode, apiCode, retryAfter(payload));
    }
    return payload;
}

std::vector<TelegramUpdate> TelegramApiClient::getUpdates(
    std::int64_t offset, long timeoutSeconds) const {
    if (offset < 0) {
        throw AppError(ErrorCode::Configuration, "Telegram update offset must not be negative");
    }
    if (timeoutSeconds <= 0) {
        throw AppError(ErrorCode::Configuration, "Telegram long-poll timeout must be positive");
    }

    const auto response = call("getUpdates", {
        {"offset", offset},
        {"limit", 100},
        {"timeout", timeoutSeconds},
        {"allowed_updates", nlohmann::json::array({"message"})},
    });
    const auto result = response.find("result");
    if (result == response.end() || !result->is_array()) {
        throw AppError(ErrorCode::Json, "Telegram getUpdates response has no result array");
    }

    std::vector<TelegramUpdate> updates;
    updates.reserve(result->size());
    for (const auto& item : *result) {
        if (!item.is_object()) {
            throw AppError(ErrorCode::Json, "Telegram update is not an object");
        }
        const auto updateId = optionalInteger(item, "update_id");
        if (!updateId || *updateId < 0) {
            throw AppError(ErrorCode::Json, "Telegram update has no valid update_id");
        }

        TelegramUpdate update{*updateId, std::nullopt};
        const auto message = item.find("message");
        if (message != item.end() && !message->is_null()) {
            update.message = parseMessage(*message);
        }
        updates.emplace_back(std::move(update));
    }
    return updates;
}

void TelegramApiClient::sendMessage(std::int64_t chatId, const std::string& text) const {
    if (text.empty()) {
        throw AppError(ErrorCode::Configuration, "Telegram message text must not be empty");
    }
    (void)call("sendMessage", {{"chat_id", chatId}, {"text", text}});
}

} // namespace trading_bot
