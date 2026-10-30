#pragma once

#include <nlohmann/json.hpp>

#include <iosfwd>
#include <mutex>
#include <string_view>

namespace trading_bot {

enum class LogLevel { Debug, Info, Warning, Error };

[[nodiscard]] LogLevel parseLogLevel(std::string_view value);
[[nodiscard]] std::string_view logLevelName(LogLevel level) noexcept;

class Logger {
public:
    explicit Logger(LogLevel minimumLevel, std::ostream& output);

    void log(LogLevel level, std::string_view event,
             const nlohmann::json& fields = nlohmann::json::object());
    void debug(std::string_view event, const nlohmann::json& fields = nlohmann::json::object());
    void info(std::string_view event, const nlohmann::json& fields = nlohmann::json::object());
    void warning(std::string_view event, const nlohmann::json& fields = nlohmann::json::object());
    void error(std::string_view event, const nlohmann::json& fields = nlohmann::json::object());

private:
    LogLevel minimumLevel_;
    std::ostream* output_;
    std::mutex mutex_;
};

} // namespace trading_bot
