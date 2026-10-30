#include "trading_bot/gemini_client.hpp"

#include "trading_bot/error.hpp"
#include "trading_bot/json_utils.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace trading_bot {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

bool validModelName(std::string_view model) {
    return !model.empty() && std::all_of(model.begin(), model.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '-' || character == '.';
    });
}

std::string apiErrorStatus(const nlohmann::json& payload) {
    if (!payload.is_object()) {
        return {};
    }
    const auto error = payload.find("error");
    if (error == payload.end() || !error->is_object()) {
        return {};
    }
    const auto status = error->find("status");
    const auto message = error->find("message");
    std::string result;
    if (status != error->end() && status->is_string()) {
        result += status->get<std::string>();
        result.push_back(' ');
    }
    if (message != error->end() && message->is_string()) {
        result += message->get<std::string>();
    }
    return lower(std::move(result));
}

GeminiFailure classifyFailure(long statusCode, const nlohmann::json& payload) {
    const auto apiStatus = apiErrorStatus(payload);
    if (apiStatus.find("resource_exhausted") != std::string::npos ||
        apiStatus.find("quota") != std::string::npos ||
        apiStatus.find("daily limit") != std::string::npos) {
        return GeminiFailure::QuotaExhausted;
    }
    if (statusCode == 429) {
        return GeminiFailure::RateLimited;
    }
    if (statusCode == 401 || statusCode == 403 ||
        apiStatus.find("unauthenticated") != std::string::npos ||
        apiStatus.find("api key") != std::string::npos ||
        apiStatus.find("api_key") != std::string::npos) {
        return GeminiFailure::InvalidCredentials;
    }
    if (statusCode == 400) {
        return GeminiFailure::InvalidRequest;
    }
    if (statusCode >= 500 || statusCode == 0) {
        return GeminiFailure::ServiceUnavailable;
    }
    return GeminiFailure::InvalidResponse;
}

std::string failureText(GeminiFailure failure) {
    switch (failure) {
    case GeminiFailure::InvalidCredentials:
        return "Gemini rejected the API credentials";
    case GeminiFailure::QuotaExhausted:
        return "Gemini API quota is exhausted";
    case GeminiFailure::RateLimited:
        return "Gemini API rate limit was reached";
    case GeminiFailure::InvalidRequest:
        return "Gemini rejected the generation request";
    case GeminiFailure::InvalidResponse:
        return "Gemini returned an invalid response";
    case GeminiFailure::ServiceUnavailable:
        return "Gemini is temporarily unavailable";
    }
    return "Gemini request failed";
}

std::string collectCandidateText(const nlohmann::json& payload) {
    if (!payload.is_object()) {
        throw GeminiApiError(GeminiFailure::InvalidResponse,
                             "Gemini returned an invalid response");
    }
    const auto feedback = payload.find("promptFeedback");
    if (feedback != payload.end() && feedback->is_object()) {
        const auto blockReason = feedback->find("blockReason");
        if (blockReason != feedback->end() && blockReason->is_string()) {
            throw GeminiApiError(GeminiFailure::InvalidResponse,
                                 "Gemini could not process the submitted context");
        }
    }

    const auto candidates = payload.find("candidates");
    if (candidates == payload.end() || !candidates->is_array() || candidates->empty() ||
        !candidates->front().is_object()) {
        throw GeminiApiError(GeminiFailure::InvalidResponse,
                             "Gemini returned no text candidate");
    }
    const auto& candidate = candidates->front();
    const auto finishReason = candidate.find("finishReason");
    if (finishReason != candidate.end() && finishReason->is_string() &&
        finishReason->get<std::string>() != "STOP") {
        throw GeminiApiError(GeminiFailure::InvalidResponse,
                             "Gemini did not finish a complete text response");
    }
    const auto content = candidate.find("content");
    if (content == candidate.end() || !content->is_object()) {
        throw GeminiApiError(GeminiFailure::InvalidResponse,
                             "Gemini returned no response content");
    }
    const auto parts = content->find("parts");
    if (parts == content->end() || !parts->is_array()) {
        throw GeminiApiError(GeminiFailure::InvalidResponse,
                             "Gemini returned no response text");
    }

    std::string output;
    for (const auto& part : *parts) {
        if (!part.is_object() || part.value("thought", false)) {
            continue;
        }
        const auto text = part.find("text");
        if (text != part.end() && text->is_string()) {
            if (!output.empty()) {
                output.push_back('\n');
            }
            output += text->get<std::string>();
        }
    }
    output = trim(output);
    if (output.empty() || output.size() > 5000) {
        throw GeminiApiError(GeminiFailure::InvalidResponse,
                             "Gemini returned empty or oversized response text");
    }
    return output;
}

} // namespace

GeminiApiError::GeminiApiError(GeminiFailure failure, std::string message)
    : std::runtime_error(std::move(message)), failure_(failure) {}

GeminiClient::GeminiClient(std::string apiKey, const HttpClientInterface& httpClient,
                           std::string model)
    : apiKey_(std::move(apiKey)), httpClient_(&httpClient), model_(std::move(model)) {
    if (apiKey_.empty() || apiKey_.find_first_of("\r\n") != std::string::npos) {
        throw AppError(ErrorCode::Configuration, "GEMINI_API_KEY is empty or invalid");
    }
    if (!validModelName(model_)) {
        throw AppError(ErrorCode::Configuration, "Gemini model name is invalid");
    }
}

std::string GeminiClient::generateText(std::string_view systemInstruction,
                                       std::string_view userData) const {
    if (systemInstruction.empty() || userData.empty() || systemInstruction.size() > 16'000 ||
        userData.size() > 64'000) {
        throw GeminiApiError(GeminiFailure::InvalidRequest,
                             "Gemini request context is empty or exceeds the size limit");
    }

    const auto payload = nlohmann::json{
        {"systemInstruction", {{"parts", {{{"text", systemInstruction}}}}}},
        {"contents", nlohmann::json::array({
            {{"role", "user"}, {"parts", nlohmann::json::array({{{"text", userData}}})}},
        })},
        {"generationConfig", {
            {"temperature", 0.2},
            {"maxOutputTokens", 500},
            {"responseMimeType", "text/plain"},
        }},
    };

    const std::string url = "https://generativelanguage.googleapis.com/v1beta/models/" +
                            model_ + ":generateContent";
    HttpResponse response;
    try {
        response = httpClient_->request(HttpRequest{
            "POST", url,
            {"Content-Type: application/json", "x-goog-api-key: " + apiKey_},
            payload.dump(),
        });
    } catch (const std::exception&) {
        throw GeminiApiError(GeminiFailure::ServiceUnavailable,
                             "Gemini request failed before a valid response was received");
    }
    if (response.body.size() > 256 * 1024) {
        throw GeminiApiError(GeminiFailure::InvalidResponse,
                             "Gemini response exceeded the size limit");
    }

    nlohmann::json parsed;
    try {
        parsed = parseJson(response.body);
    } catch (const std::exception&) {
        throw GeminiApiError(response.isSuccess() ? GeminiFailure::InvalidResponse
                                                  : classifyFailure(response.statusCode, {}),
                             response.isSuccess() ? "Gemini returned malformed JSON"
                                                  : failureText(classifyFailure(response.statusCode, {})));
    }
    if (!response.isSuccess()) {
        const auto failure = classifyFailure(response.statusCode, parsed);
        throw GeminiApiError(failure, failureText(failure));
    }
    return collectCandidateText(parsed);
}

bool GeminiClient::verifyAccess() const {
    HttpResponse response;
    try {
        response = httpClient_->request(HttpRequest{
            "GET", "https://generativelanguage.googleapis.com/v1beta/models/" + model_,
            {"Accept: application/json", "x-goog-api-key: " + apiKey_},
            {},
        });
    } catch (const std::exception&) {
        throw GeminiApiError(GeminiFailure::ServiceUnavailable,
                             "Gemini model access check failed before a valid response was received");
    }
    if (response.body.size() > 64 * 1024) {
        throw GeminiApiError(GeminiFailure::InvalidResponse,
                             "Gemini model metadata exceeded the size limit");
    }
    nlohmann::json payload;
    try {
        payload = parseJson(response.body);
    } catch (const std::exception&) {
        throw GeminiApiError(response.isSuccess() ? GeminiFailure::InvalidResponse
                                                  : classifyFailure(response.statusCode, {}),
                             response.isSuccess() ? "Gemini returned malformed model metadata"
                                                  : failureText(classifyFailure(response.statusCode, {})));
    }
    if (!response.isSuccess()) {
        const auto failure = classifyFailure(response.statusCode, payload);
        throw GeminiApiError(failure, failureText(failure));
    }
    if (!payload.is_object() || payload.value("name", std::string{}) != "models/" + model_) {
        throw GeminiApiError(GeminiFailure::InvalidResponse,
                             "Gemini returned metadata for an unexpected model");
    }
    const auto methods = payload.find("supportedGenerationMethods");
    if (methods == payload.end() || !methods->is_array() ||
        std::none_of(methods->begin(), methods->end(), [](const auto& method) {
            return method.is_string() && method.template get<std::string>() == "generateContent";
        })) {
        throw GeminiApiError(GeminiFailure::InvalidResponse,
                             "Gemini model does not advertise generateContent support");
    }
    return true;
}

} // namespace trading_bot
