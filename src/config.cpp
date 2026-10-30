#include "trading_bot/config.hpp"

#include "trading_bot/error.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace trading_bot {
namespace {

std::string trim(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

bool isValidKey(std::string_view key) {
    if (key.empty() || !(std::isalpha(static_cast<unsigned char>(key.front())) || key.front() == '_')) {
        return false;
    }
    for (const char character : key) {
        if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '_')) {
            return false;
        }
    }
    return true;
}

std::unordered_map<std::string, std::string> readDotenv(const std::filesystem::path& path) {
    std::unordered_map<std::string, std::string> values;
    std::ifstream input(path);
    if (!input) {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (!exists && !error) {
            return values;
        }
        throw AppError(ErrorCode::Configuration, "Unable to read dotenv file: " + path.string());
    }

    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const auto stripped = trim(line);
        if (stripped.empty() || stripped.front() == '#') {
            continue;
        }

        std::string_view content(stripped);
        if (content.starts_with("export ")) {
            content.remove_prefix(7);
        }
        const auto equals = content.find('=');
        if (equals == std::string_view::npos) {
            throw AppError(ErrorCode::Configuration,
                           "Invalid dotenv entry at line " + std::to_string(lineNumber));
        }
        const auto key = trim(content.substr(0, equals));
        if (!isValidKey(key)) {
            throw AppError(ErrorCode::Configuration,
                           "Invalid dotenv key at line " + std::to_string(lineNumber));
        }

        auto rawValue = trim(content.substr(equals + 1));
        std::string value;
        if (!rawValue.empty() && (rawValue.front() == '\'' || rawValue.front() == '"')) {
            const char quote = rawValue.front();
            const auto closingQuote = rawValue.find(quote, 1);
            const auto trailingValue = closingQuote == std::string::npos
                                           ? std::string{}
                                           : trim(std::string_view(rawValue).substr(closingQuote + 1));
            if (closingQuote == std::string::npos ||
                (!trailingValue.empty() && trailingValue.front() != '#')) {
                throw AppError(ErrorCode::Configuration,
                               "Invalid quoted dotenv value at line " + std::to_string(lineNumber));
            }
            value = rawValue.substr(1, closingQuote - 1);
        } else {
            const auto comment = rawValue.find(" #");
            if (comment != std::string::npos) {
                rawValue.resize(comment);
            }
            value = trim(rawValue);
        }
        values.insert_or_assign(key, std::move(value));
    }

    if (input.bad()) {
        throw AppError(ErrorCode::Configuration, "Error while reading dotenv file: " + path.string());
    }
    return values;
}

void validateSecretFilePermissions(const std::filesystem::path& path,
                                   const std::unordered_map<std::string, std::string>& values) {
    constexpr std::array<std::string_view, 5> secretKeys{
        "TELEGRAM_BOT_TOKEN", "TELEGRAM_ALLOWED_USER_IDS", "GEMINI_API_KEY",
        "COINGECKO_DEMO_API_KEY", "GOLD_API_KEY"};
    const bool containsSecret = std::any_of(secretKeys.begin(), secretKeys.end(), [&](auto key) {
        const auto found = values.find(std::string(key));
        return found != values.end() && !found->second.empty();
    });
    if (!containsSecret) {
        return;
    }
    struct stat fileInfo {};
    if (::stat(path.c_str(), &fileInfo) != 0 || fileInfo.st_uid != ::geteuid() ||
        (fileInfo.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        throw AppError(ErrorCode::Configuration,
                       "Secret-bearing dotenv file must be owned by this user and have owner-only permissions (chmod 600)");
    }
}

void overlayProcessEnvironment(std::unordered_map<std::string, std::string>& values) {
    constexpr std::array<std::string_view, 27> keys{
        "TELEGRAM_BOT_TOKEN", "TELEGRAM_ALLOWED_USER_IDS", "TELEGRAM_LONG_POLL_TIMEOUT_SECONDS",
        "GEMINI_API_KEY", "COINGECKO_DEMO_API_KEY", "GOLD_API_KEY", "BOT_DATABASE_PATH",
        "BOT_HTTP_TIMEOUT_SECONDS", "BOT_LOG_LEVEL", "BACKTEST_STARTING_BALANCE",
        "BACKTEST_FEE_BPS", "BACKTEST_SLIPPAGE_BPS", "BACKTEST_RISK_FRACTION",
        "BOT_MARKET_CHECK_INTERVAL_SECONDS", "BOT_NEWS_CHECK_INTERVAL_SECONDS",
        "BOT_NEWS_ALERTS_PER_HOUR", "BOT_NEWS_GEMINI_SUMMARIES_PER_HOUR", "BOT_TIMEZONE",
        "BOT_SIGNAL_MARKET", "BOT_SIGNAL_TIMEFRAME", "BOT_ALERT_MODE", "BOT_TRADING_MODE",
        "BOT_AI_PROVIDER", "BOT_AUTO_SIGNAL_ANALYSIS", "BOT_ALLOW_DUPLICATE_SIGNAL_ALERTS",
        "BOT_SIGNAL_ALERT_COOLDOWN_SECONDS", "BOT_SIGNAL_MIN_RISK_REWARD"};
    for (const auto key : keys) {
        if (const char* value = std::getenv(std::string(key).c_str()); value != nullptr) {
            values.insert_or_assign(std::string(key), value);
        }
    }
}

std::vector<std::int64_t> parseAllowedUserIds(std::string_view input) {
    std::vector<std::int64_t> ids;
    if (trim(input).empty()) {
        return ids;
    }

    std::size_t start = 0;
    while (start <= input.size()) {
        const auto comma = input.find(',', start);
        const auto end = comma == std::string_view::npos ? input.size() : comma;
        const auto token = trim(input.substr(start, end - start));
        std::int64_t userId = 0;
        const auto [position, error] = std::from_chars(token.data(), token.data() + token.size(), userId);
        if (token.empty() || error != std::errc{} || position != token.data() + token.size() || userId <= 0) {
            throw AppError(ErrorCode::Configuration,
                           "TELEGRAM_ALLOWED_USER_IDS must be a comma-separated list of positive integer IDs");
        }
        if (std::find(ids.begin(), ids.end(), userId) == ids.end()) {
            ids.push_back(userId);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return ids;
}

std::optional<std::string> optionalValue(
    const std::unordered_map<std::string, std::string>& values, const std::string& key) {
    const auto found = values.find(key);
    if (found == values.end() || found->second.empty()) {
        return std::nullopt;
    }
    return found->second;
}

double parseDouble(const std::unordered_map<std::string, std::string>& values,
                   const char* key, double defaultValue) {
    const auto found = values.find(key);
    if (found == values.end()) {
        return defaultValue;
    }
    double value = 0.0;
    const auto* begin = found->second.data();
    const auto* end = begin + found->second.size();
    const auto [position, error] = std::from_chars(begin, end, value, std::chars_format::general);
    if (error != std::errc{} || position != end || !std::isfinite(value)) {
        throw AppError(ErrorCode::Configuration,
                       std::string(key) + " must be a finite number");
    }
    return value;
}

bool parseBoolean(const std::unordered_map<std::string, std::string>& values,
                  const char* key, bool defaultValue) {
    const auto found = values.find(key);
    if (found == values.end()) return defaultValue;
    std::string value = found->second;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (value == "TRUE" || value == "1" || value == "YES") return true;
    if (value == "FALSE" || value == "0" || value == "NO") return false;
    throw AppError(ErrorCode::Configuration,
                   std::string(key) + " must be true or false");
}

std::string parseChoice(const std::unordered_map<std::string, std::string>& values,
                        const char* key, std::string defaultValue,
                        std::initializer_list<std::string_view> accepted) {
    const auto found = values.find(key);
    if (found == values.end()) return defaultValue;
    std::string value = found->second;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (std::find(accepted.begin(), accepted.end(), value) == accepted.end()) {
        throw AppError(ErrorCode::Configuration,
                       std::string(key) + " has an unsupported value");
    }
    return value;
}

} // namespace

Config Config::load(const std::filesystem::path& dotenvPath) {
    auto values = readDotenv(dotenvPath);
    validateSecretFilePermissions(dotenvPath, values);
    overlayProcessEnvironment(values);
    return fromValues(values);
}

Config Config::fromValues(const std::unordered_map<std::string, std::string>& values) {
    Config config;
    config.telegramBotToken = optionalValue(values, "TELEGRAM_BOT_TOKEN");
    if (const auto allowed = values.find("TELEGRAM_ALLOWED_USER_IDS"); allowed != values.end()) {
        config.telegramAllowedUserIds = parseAllowedUserIds(allowed->second);
    }
    config.geminiApiKey = optionalValue(values, "GEMINI_API_KEY");
    config.coingeckoDemoApiKey = optionalValue(values, "COINGECKO_DEMO_API_KEY");
    config.goldApiKey = optionalValue(values, "GOLD_API_KEY");
    config.signalMarket = parseChoice(values, "BOT_SIGNAL_MARKET", "XAU/USD", {"XAU/USD"});
    config.signalTimeframe = parseChoice(values, "BOT_SIGNAL_TIMEFRAME", "M5",
                                          {"M5", "M15", "H1", "H4"});
    config.alertMode = parseChoice(values, "BOT_ALERT_MODE", "CONFIRMED_ONLY",
                                    {"CONFIRMED_ONLY", "OFF"});
    config.tradingMode = parseChoice(values, "BOT_TRADING_MODE", "PAPER_ONLY", {"PAPER_ONLY"});
    config.aiProvider = parseChoice(values, "BOT_AI_PROVIDER", "GEMINI", {"GEMINI"});
    config.automaticSignalAnalysis = parseBoolean(values, "BOT_AUTO_SIGNAL_ANALYSIS", true);
    config.allowDuplicateSignalAlerts = parseBoolean(
        values, "BOT_ALLOW_DUPLICATE_SIGNAL_ALERTS", false);
    config.signalMinimumRiskReward = parseDouble(
        values, "BOT_SIGNAL_MIN_RISK_REWARD", config.signalMinimumRiskReward);
    if (!std::isfinite(config.signalMinimumRiskReward) || config.signalMinimumRiskReward < 0.5 ||
        config.signalMinimumRiskReward > 10.0) {
        throw AppError(ErrorCode::Configuration,
                       "BOT_SIGNAL_MIN_RISK_REWARD must be between 0.5 and 10");
    }

    if (const auto found = values.find("BOT_DATABASE_PATH");
        found != values.end() && !found->second.empty()) {
        config.databasePath = found->second;
    }

    if (const auto found = values.find("BOT_HTTP_TIMEOUT_SECONDS"); found != values.end()) {
        long timeout = 0;
        const auto* begin = found->second.data();
        const auto* end = begin + found->second.size();
        const auto [position, error] = std::from_chars(begin, end, timeout);
        if (error != std::errc{} || position != end || timeout <= 0) {
            throw AppError(ErrorCode::Configuration,
                           "BOT_HTTP_TIMEOUT_SECONDS must be a positive integer");
        }
        config.httpTimeoutSeconds = timeout;
    }

    if (const auto found = values.find("TELEGRAM_LONG_POLL_TIMEOUT_SECONDS"); found != values.end()) {
        long timeout = 0;
        const auto* begin = found->second.data();
        const auto* end = begin + found->second.size();
        const auto [position, error] = std::from_chars(begin, end, timeout);
        if (error != std::errc{} || position != end || timeout <= 0 || timeout > 300) {
            throw AppError(ErrorCode::Configuration,
                           "TELEGRAM_LONG_POLL_TIMEOUT_SECONDS must be an integer from 1 to 300");
        }
        config.telegramLongPollTimeoutSeconds = timeout;
    }

    const auto parseInterval = [&](const char* key, long defaultValue, long minimum) {
        const auto found = values.find(key);
        if (found == values.end()) {
            return defaultValue;
        }
        long interval = 0;
        const auto* begin = found->second.data();
        const auto* end = begin + found->second.size();
        const auto [position, error] = std::from_chars(begin, end, interval);
        if (error != std::errc{} || position != end || interval < minimum || interval > 86'400) {
            throw AppError(ErrorCode::Configuration,
                           std::string(key) + " must be an integer from " +
                               std::to_string(minimum) + " to 86400 seconds");
        }
        return interval;
    };
    config.marketCheckIntervalSeconds = parseInterval(
        "BOT_MARKET_CHECK_INTERVAL_SECONDS", config.marketCheckIntervalSeconds, 60);
    config.newsCheckIntervalSeconds = parseInterval(
        "BOT_NEWS_CHECK_INTERVAL_SECONDS", config.newsCheckIntervalSeconds, 300);
    if (const auto found = values.find("BOT_NEWS_ALERTS_PER_HOUR"); found != values.end()) {
        long maximumPerHour = 0;
        const auto* begin = found->second.data();
        const auto* end = begin + found->second.size();
        const auto [position, error] = std::from_chars(begin, end, maximumPerHour);
        if (error != std::errc{} || position != end || maximumPerHour < 1 || maximumPerHour > 120) {
            throw AppError(ErrorCode::Configuration,
                           "BOT_NEWS_ALERTS_PER_HOUR must be an integer from 1 to 120");
        }
        config.newsNotificationsPerHour = maximumPerHour;
    }
    if (const auto found = values.find("BOT_NEWS_GEMINI_SUMMARIES_PER_HOUR"); found != values.end()) {
        long maximumSummaries = 0;
        const auto* begin = found->second.data();
        const auto* end = begin + found->second.size();
        const auto [position, error] = std::from_chars(begin, end, maximumSummaries);
        if (error != std::errc{} || position != end || maximumSummaries < 1 || maximumSummaries > 120) {
            throw AppError(ErrorCode::Configuration,
                           "BOT_NEWS_GEMINI_SUMMARIES_PER_HOUR must be an integer from 1 to 120");
        }
        config.newsGeminiSummariesPerHour = maximumSummaries;
    }
    config.signalAlertCooldownSeconds = parseInterval(
        "BOT_SIGNAL_ALERT_COOLDOWN_SECONDS", config.signalAlertCooldownSeconds, 0);
    if (const auto found = values.find("BOT_TIMEZONE"); found != values.end()) {
        const auto& zone = found->second;
        const bool valid = !zone.empty() && zone.size() <= 64 &&
            std::all_of(zone.begin(), zone.end(), [](unsigned char character) {
                return (character >= 'A' && character <= 'Z') ||
                       (character >= 'a' && character <= 'z') ||
                       (character >= '0' && character <= '9') || character == '_' ||
                       character == '-' || character == '+' || character == '/';
            }) && zone.find("..") == std::string::npos && zone.front() != '/';
        if (!valid) {
            throw AppError(ErrorCode::Configuration,
                           "BOT_TIMEZONE must be a valid IANA timezone name such as UTC or Asia/Phnom_Penh");
        }
        config.timezone = zone;
    }

    if (const auto found = values.find("BOT_LOG_LEVEL"); found != values.end()) {
        config.logLevel = parseLogLevel(found->second);
    }

    config.backtestStartingBalance = parseDouble(values, "BACKTEST_STARTING_BALANCE",
                                                  config.backtestStartingBalance);
    config.backtestFeeBasisPoints = parseDouble(values, "BACKTEST_FEE_BPS",
                                                 config.backtestFeeBasisPoints);
    config.backtestSlippageBasisPoints = parseDouble(values, "BACKTEST_SLIPPAGE_BPS",
                                                      config.backtestSlippageBasisPoints);
    config.backtestRiskFraction = parseDouble(values, "BACKTEST_RISK_FRACTION",
                                               config.backtestRiskFraction);
    if (config.backtestStartingBalance <= 0.0 || config.backtestStartingBalance > 1.0e12) {
        throw AppError(ErrorCode::Configuration,
                       "BACKTEST_STARTING_BALANCE must be greater than zero and at most 1e12");
    }
    if (config.backtestFeeBasisPoints < 0.0 || config.backtestFeeBasisPoints > 1000.0) {
        throw AppError(ErrorCode::Configuration,
                       "BACKTEST_FEE_BPS must be between 0 and 1000");
    }
    if (config.backtestSlippageBasisPoints < 0.0 || config.backtestSlippageBasisPoints > 1000.0) {
        throw AppError(ErrorCode::Configuration,
                       "BACKTEST_SLIPPAGE_BPS must be between 0 and 1000");
    }
    if (config.backtestRiskFraction <= 0.0 || config.backtestRiskFraction > 0.10) {
        throw AppError(ErrorCode::Configuration,
                       "BACKTEST_RISK_FRACTION must be greater than zero and at most 0.10");
    }
    return config;
}

} // namespace trading_bot
