cmake_minimum_required(VERSION 3.20)

project(gemini_telegram_trading_bot
    VERSION 0.1.0
    DESCRIPTION "C++20 Telegram bot with free-tier market data and allowlisted long polling"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(LibXml2 REQUIRED)

# SQLite3::SQLite3 is available in newer CMake versions. Keep the 3.20
# minimum usable by aliasing the legacy target on older CMake releases.
if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()

add_library(trading_bot_core
    src/backtest_engine.cpp
    src/config.cpp
    src/database.cpp
    src/economic_calendar.cpp
    src/financial_insight.cpp
    src/financial_news.cpp
    src/gemini_client.cpp
    src/gold_market_analysis.cpp
    src/http_client.cpp
    src/integration_verifier.cpp
    src/json_utils.cpp
    src/logger.cpp
    src/market_data.cpp
    src/market_data_format.cpp
    src/monitoring_scheduler.cpp
    src/provider_market_data_service.cpp
    src/signal_engine.cpp
    src/technical_analysis.cpp
    src/telegram_api.cpp
    src/telegram_bot.cpp
    src/telegram_messages.cpp
)

target_include_directories(trading_bot_core PUBLIC include)
target_link_libraries(trading_bot_core
    PUBLIC
        CURL::libcurl
        SQLite3::SQLite3
        nlohmann_json::nlohmann_json
        LibXml2::LibXml2
)
target_compile_features(trading_bot_core PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(trading_bot_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(trading_bot src/main.cpp)
target_link_libraries(trading_bot PRIVATE trading_bot_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(trading_bot_tests tests/test_main.cpp)
    target_link_libraries(trading_bot_tests PRIVATE trading_bot_core)
    add_test(NAME trading_bot_tests COMMAND trading_bot_tests)
    add_test(NAME phase8_scheduler_integration
             COMMAND trading_bot_tests --phase8-integration)
endif()
