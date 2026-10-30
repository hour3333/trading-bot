#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace trading_bot {

struct HttpRequest {
    std::string method{"GET"};
    std::string url;
    std::vector<std::string> headers;
    std::string body;
};

struct HttpResponse {
    long statusCode{0};
    std::string body;
    [[nodiscard]] bool isSuccess() const noexcept {
        return statusCode >= 200 && statusCode < 300;
    }
};

class HttpClientInterface {
public:
    virtual ~HttpClientInterface() = default;
    [[nodiscard]] virtual HttpResponse request(const HttpRequest& request) const = 0;
};

class HttpClient final : public HttpClientInterface {
public:
    explicit HttpClient(long timeoutSeconds = 20);

    [[nodiscard]] HttpResponse request(const HttpRequest& request) const override;
    [[nodiscard]] HttpResponse requestJson(
        std::string method,
        std::string url,
        const nlohmann::json& body,
        std::vector<std::string> headers = {}) const;

private:
    long timeoutSeconds_;
};

} // namespace trading_bot
