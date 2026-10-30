#include "trading_bot/economic_calendar.hpp"

#include "trading_bot/database.hpp"
#include "trading_bot/error.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace trading_bot {
namespace {

using Clock = std::chrono::system_clock;
constexpr std::string_view kCalendarCacheKey = "bls:official-economic-release-calendar";
constexpr std::string_view kCalendarUrl = "https://www.bls.gov/schedule/news_release/bls.ics";
constexpr std::size_t kMaximumCalendarBytes = 1024 * 1024;

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

int parseDigits(std::string_view value, std::size_t offset, std::size_t count) {
    if (offset + count > value.size()) {
        throw AppError(ErrorCode::Json, "BLS calendar contains an invalid event timestamp");
    }
    int result = 0;
    const auto* first = value.data() + offset;
    const auto* last = first + count;
    const auto [position, error] = std::from_chars(first, last, result);
    if (error != std::errc{} || position != last) {
        throw AppError(ErrorCode::Json, "BLS calendar contains an invalid event timestamp");
    }
    return result;
}

std::string unescapeText(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '\\' && index + 1 < input.size()) {
            const char escaped = input[++index];
            if (escaped == 'n' || escaped == 'N') {
                result.push_back(' ');
            } else {
                result.push_back(escaped);
            }
        } else {
            result.push_back(input[index]);
        }
    }
    return result;
}

std::string trim(std::string_view input) {
    std::size_t first = 0;
    std::size_t last = input.size();
    while (first < last && std::isspace(static_cast<unsigned char>(input[first]))) {
        ++first;
    }
    while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1]))) {
        --last;
    }
    return std::string(input.substr(first, last - first));
}

int sundayOfFirst(std::chrono::year yearValue, std::chrono::month monthValue) {
    const std::chrono::year_month_day first{yearValue, monthValue, std::chrono::day{1}};
    const unsigned weekday = std::chrono::weekday{std::chrono::sys_days{first}}.c_encoding();
    return 1 + static_cast<int>((7U - weekday) % 7U);
}

bool isNewYorkDaylightTime(const std::chrono::year_month_day& date, int hour) {
    const auto month = static_cast<unsigned>(date.month());
    const auto day = static_cast<unsigned>(date.day());
    const int marchSecondSunday = sundayOfFirst(date.year(), std::chrono::March) + 7;
    const int novemberFirstSunday = sundayOfFirst(date.year(), std::chrono::November);
    if (month < 3 || month > 11) {
        return false;
    }
    if (month > 3 && month < 11) {
        return true;
    }
    if (month == 3) {
        return day > static_cast<unsigned>(marchSecondSunday) ||
               (day == static_cast<unsigned>(marchSecondSunday) && hour >= 2);
    }
    return day < static_cast<unsigned>(novemberFirstSunday) ||
           (day == static_cast<unsigned>(novemberFirstSunday) && hour < 2);
}

struct ParsedEventTime {
    Clock::time_point timestamp;
    std::string localText;
    std::string timezone;
    bool allDay{false};
};

ParsedEventTime parseEventTime(std::string_view raw, std::string timezone, bool allDay) {
    const bool utcSuffix = !allDay && !raw.empty() && raw.back() == 'Z';
    const auto value = utcSuffix ? raw.substr(0, raw.size() - 1) : raw;
    if (value.size() != (allDay ? 8U : 15U) || (!allDay && value[8] != 'T')) {
        throw AppError(ErrorCode::Json, "BLS calendar contains an unsupported event timestamp");
    }
    const std::chrono::year_month_day date{
        std::chrono::year{parseDigits(value, 0, 4)},
        std::chrono::month{static_cast<unsigned>(parseDigits(value, 4, 2))},
        std::chrono::day{static_cast<unsigned>(parseDigits(value, 6, 2))},
    };
    int hour = allDay ? 12 : parseDigits(value, 9, 2);
    const int minute = allDay ? 0 : parseDigits(value, 11, 2);
    const int second = allDay ? 0 : parseDigits(value, 13, 2);
    if (!date.ok() || hour > 23 || minute > 59 || second > 59) {
        throw AppError(ErrorCode::Json, "BLS calendar contains an out-of-range event timestamp");
    }

    if (utcSuffix || timezone.empty() || timezone == "UTC" || timezone == "Etc/UTC" || timezone == "GMT") {
        timezone = "UTC";
    } else if (timezone == "US/Eastern") {
        timezone = "America/New_York";
    } else if (timezone != "America/New_York") {
        throw AppError(ErrorCode::Json, "BLS calendar contains an unsupported timezone");
    }

    auto seconds = std::chrono::sys_days{date}.time_since_epoch() + std::chrono::hours{hour} +
                   std::chrono::minutes{minute} + std::chrono::seconds{second};
    if (timezone == "America/New_York") {
        const auto offsetHours = isNewYorkDaylightTime(date, hour) ? 4 : 5;
        seconds += std::chrono::hours{offsetHours};
    }
    const auto timestamp = Clock::time_point{std::chrono::duration_cast<Clock::duration>(seconds)};

    std::ostringstream localText;
    localText << std::setfill('0') << std::setw(4) << static_cast<int>(date.year()) << '-'
              << std::setw(2) << static_cast<unsigned>(date.month()) << '-'
              << std::setw(2) << static_cast<unsigned>(date.day());
    if (!allDay) {
        localText << ' ' << std::setw(2) << hour << ':' << std::setw(2) << minute;
    }
    return {timestamp, localText.str(), timezone, allDay};
}

bool isSafeHttpsUrl(std::string_view url) {
    if (!url.starts_with("https://") || url.size() > 2048) {
        return false;
    }
    return std::none_of(url.begin(), url.end(), [](unsigned char character) {
        return character <= 0x20 || character == 0x7f;
    });
}

struct PendingEvent {
    std::string uid;
    std::string title;
    std::string url;
    std::string timezone;
    std::string start;
    bool allDay{false};
    bool cancelled{false};
    bool hasStart{false};
};

std::vector<std::string> unfoldedLines(std::string_view input) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < input.size()) {
        const auto newline = input.find('\n', start);
        const auto end = newline == std::string_view::npos ? input.size() : newline;
        auto line = std::string(input.substr(start, end - start));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && (line.front() == ' ' || line.front() == '\t') && !lines.empty()) {
            lines.back().append(line.begin() + 1, line.end());
        } else {
            lines.push_back(std::move(line));
        }
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }
    return lines;
}

std::vector<EconomicCalendarEvent> parseCalendar(std::string_view body, Clock::time_point asOf) {
    if (body.empty() || body.size() > kMaximumCalendarBytes ||
        body.find("BEGIN:VCALENDAR") == std::string_view::npos) {
        throw AppError(ErrorCode::Json, "BLS calendar response is empty or not an iCalendar document");
    }
    std::vector<EconomicCalendarEvent> events;
    std::set<std::string> seen;
    PendingEvent pending;
    bool inEvent = false;
    const auto finishEvent = [&] {
        if (pending.cancelled || pending.title.empty() || !pending.hasStart) {
            return;
        }
        const auto parsed = parseEventTime(pending.start, pending.timezone, pending.allDay);
        if (parsed.timestamp <= asOf || parsed.timestamp > asOf + std::chrono::hours(24 * 370)) {
            return;
        }
        if (!pending.url.empty() && !isSafeHttpsUrl(pending.url)) {
            throw AppError(ErrorCode::Json, "BLS calendar contains an unsafe event URL");
        }
        const std::string identity = pending.uid.empty()
            ? pending.title + "|" + parsed.localText : pending.uid;
        if (!seen.insert(identity).second) {
            return;
        }
        events.push_back({pending.title, parsed.timestamp, parsed.localText, parsed.timezone,
                          pending.url.empty() ? "https://www.bls.gov/schedule/news_release/" : pending.url,
                          parsed.allDay});
    };

    for (const auto& line : unfoldedLines(body)) {
        if (line == "BEGIN:VEVENT") {
            pending = {};
            inEvent = true;
            continue;
        }
        if (line == "END:VEVENT") {
            if (inEvent) {
                finishEvent();
            }
            inEvent = false;
            continue;
        }
        if (!inEvent) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const auto property = std::string_view(line).substr(0, colon);
        const auto rawValue = std::string_view(line).substr(colon + 1);
        const auto separator = property.find(';');
        const auto propertyName = upper(std::string(property.substr(0, separator)));
        if (propertyName == "UID") {
            pending.uid = trim(rawValue);
        } else if (propertyName == "SUMMARY") {
            pending.title = trim(unescapeText(rawValue));
            if (pending.title.size() > 300) {
                pending.title.resize(300);
            }
        } else if (propertyName == "URL") {
            pending.url = trim(unescapeText(rawValue));
        } else if (propertyName == "STATUS") {
            pending.cancelled = upper(trim(rawValue)) == "CANCELLED";
        } else if (propertyName == "DTSTART") {
            pending.start = trim(rawValue);
            pending.hasStart = true;
            pending.allDay = pending.start.size() == 8;
            if (separator != std::string_view::npos) {
                auto parameters = property.substr(separator + 1);
                std::size_t parameterStart = 0;
                while (parameterStart <= parameters.size()) {
                    const auto next = parameters.find(';', parameterStart);
                    const auto parameterEnd = next == std::string_view::npos ? parameters.size() : next;
                    const auto parameter = parameters.substr(parameterStart, parameterEnd - parameterStart);
                    if (parameter.starts_with("TZID=")) {
                        pending.timezone = std::string(parameter.substr(5));
                        if (pending.timezone.size() >= 2 && pending.timezone.front() == '"' &&
                            pending.timezone.back() == '"') {
                            pending.timezone = pending.timezone.substr(1, pending.timezone.size() - 2);
                        }
                    } else if (parameter == "VALUE=DATE") {
                        pending.allDay = true;
                    }
                    if (next == std::string_view::npos) {
                        break;
                    }
                    parameterStart = next + 1;
                }
            }
            if (!pending.start.empty() && pending.start.back() == 'Z') {
                pending.timezone = "UTC";
            }
        }
    }
    std::stable_sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
        return left.startAt < right.startAt;
    });
    return events;
}

std::string formatUtc(Clock::time_point value) {
    const auto raw = Clock::to_time_t(value);
    std::tm utc{};
    gmtime_r(&raw, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%d %H:%M UTC");
    return output.str();
}

EconomicCalendarSnapshot fromBody(std::string_view body, Clock::time_point fetchedAt,
                                  Clock::time_point asOf, std::size_t limit, bool delayed) {
    auto events = parseCalendar(body, asOf);
    if (events.size() > limit) {
        events.resize(limit);
    }
    return {std::move(events), fetchedAt, delayed, false};
}

} // namespace

BlsEconomicCalendarService::BlsEconomicCalendarService(
    Database& database, const HttpClientInterface& httpClient, std::chrono::seconds cacheTtl)
    : database_(&database), httpClient_(&httpClient), cacheTtl_(cacheTtl) {
    if (cacheTtl_ <= std::chrono::seconds::zero()) {
        throw AppError(ErrorCode::Configuration, "BLS calendar cache interval must be positive");
    }
}

EconomicCalendarSnapshot BlsEconomicCalendarService::getUpcomingEvents(
    std::size_t limit, Clock::time_point asOf) {
    EconomicCalendarSnapshot unavailable;
    unavailable.fetchedAt = asOf;
    if (limit == 0) {
        return unavailable;
    }
    limit = std::min<std::size_t>(limit, 30);
    const auto cached = database_->loadProviderCache(kCalendarCacheKey);
    if (cached) {
        const auto age = asOf - cached->fetchedAt;
        if (age >= Clock::duration::zero() && age <= cacheTtl_) {
            try {
                return fromBody(cached->payload, cached->fetchedAt, asOf, limit, false);
            } catch (const std::exception&) {
                // Fetch a replacement for malformed cached calendar content.
            }
        }
    }

    try {
        const auto response = httpClient_->request(HttpRequest{
            "GET", std::string(kCalendarUrl),
            {"Accept: text/calendar, text/plain", "User-Agent: CppTradingBot/1.0 BLS calendar reader"},
            {},
        });
        if (!response.isSuccess() || response.body.size() > kMaximumCalendarBytes) {
            throw AppError(ErrorCode::Network, "BLS calendar endpoint did not return a valid response");
        }
        const auto fetchedAt = asOf;
        auto snapshot = fromBody(response.body, fetchedAt, asOf, limit, false);
        database_->saveProviderCache(kCalendarCacheKey, response.body, fetchedAt);
        return snapshot;
    } catch (const std::exception&) {
        if (cached) {
            try {
                return fromBody(cached->payload, cached->fetchedAt, asOf, limit, true);
            } catch (const std::exception&) {
                // Continue to an explicit unavailable result.
            }
        }
        unavailable.unavailable = true;
        return unavailable;
    }
}

std::string formatEconomicCalendarMessage(const EconomicCalendarSnapshot& snapshot,
                                          std::size_t maxEvents, std::size_t maxCharacters) {
    maxCharacters = std::clamp<std::size_t>(maxCharacters, 200, 3500);
    std::ostringstream output;
    output << "BLS ECONOMIC RELEASE CALENDAR (official schedule)\n"
           << "Checked: " << formatUtc(snapshot.fetchedAt) << '\n';
    if (snapshot.unavailable) {
        output << "ECONOMIC CALENDAR DATA UNAVAILABLE: the BLS schedule could not be validated.\n";
        return output.str();
    }
    if (snapshot.delayed) {
        output << "DATA DELAYED: showing the last validated cached calendar.\n";
    }
    if (snapshot.events.empty()) {
        output << "No upcoming BLS events were found in the available schedule.\n";
    } else {
        const auto count = std::min(maxEvents, snapshot.events.size());
        for (std::size_t index = 0; index < count; ++index) {
            const auto& event = snapshot.events[index];
            std::ostringstream entry;
            entry << '\n' << index + 1 << ". " << event.title << '\n'
                  << "Scheduled: " << event.localTime;
            if (!event.allDay) {
                entry << ' ' << event.timezone;
            } else {
                entry << " (all day)";
            }
            entry << '\n' << event.url << '\n';
            const auto text = entry.str();
            if (output.tellp() >= 0 && static_cast<std::size_t>(output.tellp()) + text.size() > maxCharacters) {
                break;
            }
            output << text;
        }
    }
    output << "Scheduled dates can change; check the BLS source before relying on event timing.\n"
           << "https://www.bls.gov/schedule/news_release/";
    auto result = output.str();
    if (result.size() > maxCharacters) {
        result.resize(maxCharacters - 3);
        result += "...";
    }
    return result;
}

} // namespace trading_bot
