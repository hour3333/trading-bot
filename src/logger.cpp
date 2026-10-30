#include "trading_bot/logger.hpp"

#include "trading_bot/error.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>

namespace trading_bot {
namespace {

int severity(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return 0;
    case LogLevel::Info:
        return 1;
    case LogLevel::Warning:
        return 2;
    case LogLevel::Error:
        return 3;
    }
    return 4;
}

std::string timestampUtc() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now.time_since_epoch()) % 1000;
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3)
           << std::setfill('0') << milliseconds.count() << 'Z';
    return output.str();
}

} // namespace

LogLevel parseLogLevel(std::string_view value) {
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (normalized == "debug") {
        return LogLevel::Debug;
    }
    if (normalized == "info") {
        return LogLevel::Info;
    }
    if (normalized == "warning" || normalized == "warn") {
        return LogLevel::Warning;
    }
    if (normalized == "error") {
        return LogLevel::Error;
    }
    throw AppError(ErrorCode::Configuration, "BOT_LOG_LEVEL must be debug, info, warning, or error");
}

std::string_view logLevelName(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warning";
    case LogLevel::Error:
        return "error";
    }
    return "unknown";
}

Logger::Logger(LogLevel minimumLevel, std::ostream& output)
    : minimumLevel_(minimumLevel), output_(&output) {}

void Logger::log(LogLevel level, std::string_view event, const nlohmann::json& fields) {
    if (severity(level) < severity(minimumLevel_)) {
        return;
    }
    nlohmann::json entry{
        {"timestamp", timestampUtc()},
        {"level", logLevelName(level)},
        {"event", event},
        {"fields", fields},
    };
    std::lock_guard lock(mutex_);
    *output_ << entry.dump() << '\n';
}

void Logger::debug(std::string_view event, const nlohmann::json& fields) {
    log(LogLevel::Debug, event, fields);
}

void Logger::info(std::string_view event, const nlohmann::json& fields) {
    log(LogLevel::Info, event, fields);
}

void Logger::warning(std::string_view event, const nlohmann::json& fields) {
    log(LogLevel::Warning, event, fields);
}

void Logger::error(std::string_view event, const nlohmann::json& fields) {
    log(LogLevel::Error, event, fields);
}

} // namespace trading_bot
