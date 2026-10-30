#pragma once

#include "trading_bot/config.hpp"

#include <string>
#include <vector>

namespace trading_bot {

class Database;
class HttpClientInterface;

struct IntegrationCheck {
    std::string service;
    std::string status;
    std::string detail;
};

// Performs read-only credential/data checks. It never sends Telegram messages
// or invokes Gemini content generation.
[[nodiscard]] std::vector<IntegrationCheck> verifyIntegrations(
    const Config& config, Database& temporaryDatabase,
    const HttpClientInterface& httpClient);
[[nodiscard]] std::string formatIntegrationReport(
    const std::vector<IntegrationCheck>& checks);

} // namespace trading_bot
