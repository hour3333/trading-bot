#pragma once

#include "trading_bot/http_client.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace trading_bot {

class Database;

struct EconomicCalendarEvent {
    std::string title;
    std::chrono::system_clock::time_point startAt;
    std::string localTime;
    std::string timezone;
    std::string url;
    bool allDay{false};
};

struct EconomicCalendarSnapshot {
    std::vector<EconomicCalendarEvent> events;
    std::chrono::system_clock::time_point fetchedAt{};
    bool delayed{false};
    bool unavailable{false};
};

class EconomicCalendarService {
public:
    virtual ~EconomicCalendarService() = default;
    [[nodiscard]] virtual EconomicCalendarSnapshot getUpcomingEvents(
        std::size_t limit = 10,
        std::chrono::system_clock::time_point asOf = std::chrono::system_clock::now()) = 0;
};

// Consumes the official public BLS release calendar (RFC 5545 iCalendar).
class BlsEconomicCalendarService final : public EconomicCalendarService {
public:
    BlsEconomicCalendarService(Database& database, const HttpClientInterface& httpClient,
                               std::chrono::seconds cacheTtl = std::chrono::hours(6));
    [[nodiscard]] EconomicCalendarSnapshot getUpcomingEvents(
        std::size_t limit = 10,
        std::chrono::system_clock::time_point asOf = std::chrono::system_clock::now()) override;

private:
    Database* database_;
    const HttpClientInterface* httpClient_;
    std::chrono::seconds cacheTtl_;
};

[[nodiscard]] std::string formatEconomicCalendarMessage(
    const EconomicCalendarSnapshot& snapshot, std::size_t maxEvents = 10,
    std::size_t maxCharacters = 3000);

} // namespace trading_bot
