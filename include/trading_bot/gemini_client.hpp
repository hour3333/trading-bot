#pragma once

#include "trading_bot/http_client.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace trading_bot {

enum class GeminiFailure {
    InvalidCredentials,
    QuotaExhausted,
    RateLimited,
    InvalidRequest,
    InvalidResponse,
    ServiceUnavailable,
};

class GeminiApiError final : public std::runtime_error {
public:
    GeminiApiError(GeminiFailure failure, std::string message);
    [[nodiscard]] GeminiFailure failure() const noexcept { return failure_; }

private:
    GeminiFailure failure_;
};

class GeminiClient final {
public:
    static constexpr std::string_view kDefaultModel = "gemini-3.8-flash";

    GeminiClient(std::string apiKey, const HttpClientInterface& httpClient,
                 std::string model = std::string(kDefaultModel));

    // The system instruction and untrusted user data are sent as separate API
    // fields. API keys are sent only in the x-goog-api-key header.
    [[nodiscard]] std::string generateText(std::string_view systemInstruction,
                                           std::string_view userData) const;
    // Performs only a GET models/{model} metadata request. This checks key and
    // model access without generating content or consuming model tokens.
    [[nodiscard]] bool verifyAccess() const;
    [[nodiscard]] const std::string& model() const noexcept { return model_; }

private:
    std::string apiKey_;
    const HttpClientInterface* httpClient_;
    std::string model_;
};

} // namespace trading_bot
