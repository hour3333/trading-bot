#include "trading_bot/http_client.hpp"

#include "trading_bot/error.hpp"

#include <curl/curl.h>

#include <limits>
#include <memory>
#include <mutex>
#include <string>

namespace trading_bot {
namespace {

void ensureCurlInitialized() {
    static std::once_flag initializationFlag;
    static CURLcode initializationResult = CURLE_OK;
    std::call_once(initializationFlag, [] { initializationResult = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (initializationResult != CURLE_OK) {
        throw AppError(ErrorCode::Network,
                       std::string("Unable to initialize libcurl: ") + curl_easy_strerror(initializationResult));
    }
}

std::size_t appendResponse(char* data, std::size_t size, std::size_t count, void* userData) noexcept {
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        return 0;
    }
    const auto byteCount = size * count;
    try {
        static_cast<std::string*>(userData)->append(data, byteCount);
    } catch (...) {
        return 0;
    }
    return byteCount;
}

} // namespace

HttpClient::HttpClient(long timeoutSeconds) : timeoutSeconds_(timeoutSeconds) {
    if (timeoutSeconds_ <= 0) {
        throw AppError(ErrorCode::Configuration, "HTTP timeout must be a positive number of seconds");
    }
    ensureCurlInitialized();
}

HttpResponse HttpClient::request(const HttpRequest& requestData) const {
    if (requestData.url.empty()) {
        throw AppError(ErrorCode::Configuration, "HTTP request URL must not be empty");
    }
    if (requestData.method.empty()) {
        throw AppError(ErrorCode::Configuration, "HTTP request method must not be empty");
    }
    ensureCurlInitialized();

    CURL* rawHandle = curl_easy_init();
    if (rawHandle == nullptr) {
        throw AppError(ErrorCode::Network, "Unable to create a libcurl request handle");
    }
    const auto cleanupHandle = [](CURL* handle) { curl_easy_cleanup(handle); };
    std::unique_ptr<CURL, decltype(cleanupHandle)> handle(rawHandle, cleanupHandle);

    curl_slist* rawHeaders = nullptr;
    for (const auto& header : requestData.headers) {
        curl_slist* appended = curl_slist_append(rawHeaders, header.c_str());
        if (appended == nullptr) {
            curl_slist_free_all(rawHeaders);
            throw AppError(ErrorCode::Network, "Unable to allocate HTTP request headers");
        }
        rawHeaders = appended;
    }
    const auto cleanupHeaders = [](curl_slist* headers) { curl_slist_free_all(headers); };
    std::unique_ptr<curl_slist, decltype(cleanupHeaders)> headers(rawHeaders, cleanupHeaders);

    HttpResponse response;
    auto setOption = [&](CURLoption option, auto value, const char* name) {
        const CURLcode result = curl_easy_setopt(handle.get(), option, value);
        if (result != CURLE_OK) {
            throw AppError(ErrorCode::Network,
                           std::string("Unable to set libcurl option ") + name + ": " + curl_easy_strerror(result));
        }
    };

    setOption(CURLOPT_URL, requestData.url.c_str(), "URL");
    setOption(CURLOPT_NOSIGNAL, 1L, "NOSIGNAL");
    setOption(CURLOPT_TIMEOUT, timeoutSeconds_, "TIMEOUT");
    setOption(CURLOPT_WRITEFUNCTION, &appendResponse, "WRITEFUNCTION");
    setOption(CURLOPT_WRITEDATA, &response.body, "WRITEDATA");
    setOption(CURLOPT_CUSTOMREQUEST, requestData.method.c_str(), "CUSTOMREQUEST");
    if (headers) {
        setOption(CURLOPT_HTTPHEADER, headers.get(), "HTTPHEADER");
    }
    if (!requestData.body.empty() || requestData.method == "POST" || requestData.method == "PUT" ||
        requestData.method == "PATCH") {
        setOption(CURLOPT_POSTFIELDS, requestData.body.data(), "POSTFIELDS");
        setOption(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(requestData.body.size()), "POSTFIELDSIZE");
    }

    const CURLcode performResult = curl_easy_perform(handle.get());
    if (performResult != CURLE_OK) {
        throw AppError(ErrorCode::Network,
                       std::string("HTTP request failed: ") + curl_easy_strerror(performResult));
    }
    const CURLcode infoResult = curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &response.statusCode);
    if (infoResult != CURLE_OK) {
        throw AppError(ErrorCode::Network,
                       std::string("Unable to read HTTP status: ") + curl_easy_strerror(infoResult));
    }
    return response;
}

HttpResponse HttpClient::requestJson(std::string method, std::string url,
                                     const nlohmann::json& body,
                                     std::vector<std::string> headers) const {
    headers.emplace_back("Content-Type: application/json");
    headers.emplace_back("Accept: application/json");
    return request(HttpRequest{std::move(method), std::move(url), std::move(headers), body.dump()});
}

} // namespace trading_bot
