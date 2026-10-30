#include "trading_bot/financial_news.hpp"

#include "trading_bot/database.hpp"
#include "trading_bot/error.hpp"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace trading_bot {
namespace {

using Clock = std::chrono::system_clock;
constexpr std::size_t kMaximumFeedBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaximumHeadlineBytes = 300;
constexpr std::size_t kMaximumUrlBytes = 700;

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string truncateUtf8(std::string value, std::size_t maximumBytes) {
    if (value.size() <= maximumBytes) {
        return value;
    }
    value.resize(maximumBytes);
    while (!value.empty() &&
           (static_cast<unsigned char>(value.back()) & 0xc0U) == 0x80U) {
        value.pop_back();
    }
    if (!value.empty()) {
        const auto lead = static_cast<unsigned char>(value.back());
        if ((lead & 0xe0U) == 0xc0U || (lead & 0xf0U) == 0xe0U || (lead & 0xf8U) == 0xf0U) {
            value.pop_back();
        }
    }
    return value;
}

std::optional<int> parseInt(std::string_view text) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<unsigned> monthNumber(std::string_view month) {
    constexpr std::array<std::string_view, 12> months{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    for (std::size_t index = 0; index < months.size(); ++index) {
        if (month == months[index]) {
            return static_cast<unsigned>(index + 1);
        }
    }
    return std::nullopt;
}

std::optional<int> zoneOffsetMinutes(std::string_view zone) {
    if (zone == "Z" || zone == "UTC" || zone == "UT" || zone == "GMT") {
        return 0;
    }
    if (zone.size() != 5 && zone.size() != 6) {
        return std::nullopt;
    }
    if ((zone[0] != '+' && zone[0] != '-') || (zone.size() == 6 && zone[3] != ':')) {
        return std::nullopt;
    }
    const auto hourText = zone.substr(1, 2);
    const auto minuteText = zone.size() == 6 ? zone.substr(4, 2) : zone.substr(3, 2);
    const auto hours = parseInt(hourText);
    const auto minutes = parseInt(minuteText);
    if (!hours || !minutes || *hours > 23 || *minutes > 59) {
        return std::nullopt;
    }
    const int sign = zone[0] == '-' ? -1 : 1;
    return sign * (*hours * 60 + *minutes);
}

std::optional<Clock::time_point> makeTimestamp(int year, unsigned month, unsigned day,
                                               int hour, int minute, int second,
                                               int offsetMinutes) {
    const std::chrono::year_month_day date{
        std::chrono::year(year), std::chrono::month(month), std::chrono::day(day)};
    if (!date.ok() || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) {
        return std::nullopt;
    }
    const auto local = std::chrono::sys_days(date) + std::chrono::hours(hour) +
                       std::chrono::minutes(minute) + std::chrono::seconds(second);
    const auto utc = local - std::chrono::minutes(offsetMinutes);
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(utc.time_since_epoch()));
}

std::optional<Clock::time_point> parseIsoDate(std::string_view value) {
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':') {
        return std::nullopt;
    }
    const auto year = parseInt(value.substr(0, 4));
    const auto month = parseInt(value.substr(5, 2));
    const auto day = parseInt(value.substr(8, 2));
    const auto hour = parseInt(value.substr(11, 2));
    const auto minute = parseInt(value.substr(14, 2));
    const auto second = parseInt(value.substr(17, 2));
    if (!year || !month || !day || !hour || !minute || !second) {
        return std::nullopt;
    }

    std::size_t suffixPosition = 19;
    if (suffixPosition < value.size() && value[suffixPosition] == '.') {
        ++suffixPosition;
        const auto fractionStart = suffixPosition;
        while (suffixPosition < value.size() && value[suffixPosition] >= '0' &&
               value[suffixPosition] <= '9') {
            ++suffixPosition;
        }
        if (suffixPosition == fractionStart) {
            return std::nullopt;
        }
    }
    const auto offset = zoneOffsetMinutes(value.substr(suffixPosition));
    if (!offset) {
        return std::nullopt;
    }
    return makeTimestamp(*year, static_cast<unsigned>(*month), static_cast<unsigned>(*day),
                         *hour, *minute, *second, *offset);
}

std::optional<Clock::time_point> parseRfc822Date(std::string value) {
    const auto comma = value.find(',');
    if (comma != std::string::npos) {
        value = trim(std::string_view(value).substr(comma + 1));
    }
    std::istringstream input(value);
    std::string dayText;
    std::string monthText;
    std::string yearText;
    std::string timeText;
    std::string zone;
    if (!(input >> dayText >> monthText >> yearText >> timeText >> zone)) {
        return std::nullopt;
    }
    const auto day = parseInt(dayText);
    const auto year = parseInt(yearText);
    const auto month = monthNumber(monthText);
    const auto offset = zoneOffsetMinutes(zone);
    const auto firstColon = timeText.find(':');
    const auto secondColon = firstColon == std::string::npos
                                 ? std::string::npos
                                 : timeText.find(':', firstColon + 1);
    if (!day || !year || !month || !offset || firstColon == std::string::npos ||
        secondColon == std::string::npos) {
        return std::nullopt;
    }
    const auto hour = parseInt(std::string_view(timeText).substr(0, firstColon));
    const auto minute = parseInt(std::string_view(timeText).substr(
        firstColon + 1, secondColon - firstColon - 1));
    const auto second = parseInt(std::string_view(timeText).substr(secondColon + 1));
    if (!hour || !minute || !second) {
        return std::nullopt;
    }
    return makeTimestamp(*year, *month, static_cast<unsigned>(*day), *hour, *minute,
                         *second, *offset);
}

std::optional<Clock::time_point> parsePublicationDate(std::string value) {
    value = trim(value);
    if (value.empty()) {
        return std::nullopt;
    }
    if (value.size() >= 10 && value[4] == '-') {
        return parseIsoDate(value);
    }
    return parseRfc822Date(std::move(value));
}

std::string nodeText(const xmlNode* parent, std::string_view wantedName) {
    for (auto* node = parent != nullptr ? parent->children : nullptr;
         node != nullptr; node = node->next) {
        if (node->type == XML_ELEMENT_NODE &&
            std::string_view(reinterpret_cast<const char*>(node->name)) == wantedName) {
            xmlChar* raw = xmlNodeGetContent(const_cast<xmlNode*>(node));
            if (raw == nullptr) {
                return {};
            }
            std::string result(reinterpret_cast<const char*>(raw));
            xmlFree(raw);
            return trim(result);
        }
    }
    return {};
}

std::string nodeAttribute(const xmlNode* node, const char* name) {
    if (node == nullptr) {
        return {};
    }
    xmlChar* raw = xmlGetProp(const_cast<xmlNode*>(node), BAD_CAST name);
    if (raw == nullptr) {
        return {};
    }
    std::string result(reinterpret_cast<const char*>(raw));
    xmlFree(raw);
    return trim(result);
}

std::string articleLink(const xmlNode* entry, bool atom) {
    for (auto* node = entry != nullptr ? entry->children : nullptr;
         node != nullptr; node = node->next) {
        if (node->type != XML_ELEMENT_NODE ||
            std::string_view(reinterpret_cast<const char*>(node->name)) != "link") {
            continue;
        }
        auto link = atom ? nodeAttribute(node, "href") : std::string{};
        if (link.empty()) {
            xmlChar* raw = xmlNodeGetContent(node);
            if (raw != nullptr) {
                link = trim(reinterpret_cast<const char*>(raw));
                xmlFree(raw);
            }
        }
        if (!link.empty()) {
            return link;
        }
    }
    return {};
}

bool validHttpsUrl(std::string_view url) {
    if (!url.starts_with("https://") || url.size() > kMaximumUrlBytes ||
        url.size() <= 8 || url.find_first_of("\r\n\t <>\"") != std::string_view::npos) {
        return false;
    }
    const auto authorityStart = std::string_view("https://").size();
    const auto authorityEnd = url.find_first_of("/?#", authorityStart);
    const auto authority = url.substr(authorityStart,
        authorityEnd == std::string_view::npos ? url.size() - authorityStart
                                               : authorityEnd - authorityStart);
    return !authority.empty() && authority.find('@') == std::string_view::npos;
}

std::string lowerAscii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::string_view httpsHost(std::string_view url) {
    constexpr std::string_view prefix = "https://";
    if (!url.starts_with(prefix)) {
        return {};
    }
    const auto begin = prefix.size();
    const auto end = url.find_first_of("/?#", begin);
    return url.substr(begin, end == std::string_view::npos ? url.size() - begin : end - begin);
}

std::vector<NewsArticle> parseFeed(std::string_view xml, const NewsFeed& feed,
                                   Clock::time_point asOf,
                                   std::chrono::hours maximumAge) {
    if (xml.empty() || xml.size() > kMaximumFeedBytes ||
        xml.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw AppError(ErrorCode::Json, "RSS feed is empty or exceeds the size limit");
    }
    const std::string sourceUrl = feed.url;
    constexpr int options = XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR |
                            XML_PARSE_NOWARNING | XML_PARSE_COMPACT;
    xmlDocPtr rawDocument = xmlReadMemory(xml.data(), static_cast<int>(xml.size()),
                                          sourceUrl.c_str(), nullptr, options);
    const auto freeDocument = [](xmlDoc* document) { xmlFreeDoc(document); };
    std::unique_ptr<xmlDoc, decltype(freeDocument)> document(rawDocument, freeDocument);
    if (!document || document->intSubset != nullptr || document->extSubset != nullptr) {
        throw AppError(ErrorCode::Json, "RSS feed XML could not be safely parsed");
    }
    xmlNode* root = xmlDocGetRootElement(document.get());
    if (root == nullptr) {
        throw AppError(ErrorCode::Json, "RSS feed has no document element");
    }
    const std::string_view rootName(reinterpret_cast<const char*>(root->name));
    const bool atom = rootName == "feed";
    if (!atom && rootName != "rss" && rootName != "RDF") {
        throw AppError(ErrorCode::Json, "Unsupported RSS feed document type");
    }

    std::string feedTitle;
    std::vector<const xmlNode*> entries;
    for (auto* node = root->children; node != nullptr; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) {
            continue;
        }
        const std::string_view name(reinterpret_cast<const char*>(node->name));
        if (name == "channel") {
            feedTitle = nodeText(node, "title");
            for (auto* child = node->children; child != nullptr; child = child->next) {
                if (child->type == XML_ELEMENT_NODE &&
                    std::string_view(reinterpret_cast<const char*>(child->name)) == "item") {
                    entries.push_back(child);
                }
            }
        } else if (name == "entry" || name == "item") {
            entries.push_back(node);
        }
    }
    if (atom) {
        feedTitle = nodeText(root, "title");
    }
    if (feedTitle.empty()) {
        feedTitle = feed.source;
    }

    std::vector<NewsArticle> articles;
    articles.reserve(entries.size());
    for (const auto* entry : entries) {
        auto headline = nodeText(entry, "title");
        auto url = articleLink(entry, atom);
        auto dateText = atom ? nodeText(entry, "published") : nodeText(entry, "pubDate");
        if (dateText.empty()) {
            dateText = nodeText(entry, "updated");
        }
        auto publishedAt = parsePublicationDate(std::move(dateText));
        if (headline.empty() || headline.size() > 4096 || !publishedAt ||
            !validHttpsUrl(url) || lowerAscii(httpsHost(url)) != lowerAscii(httpsHost(feed.url)) ||
            *publishedAt > asOf + std::chrono::minutes(5) ||
            asOf - *publishedAt > maximumAge) {
            continue;
        }

        auto source = nodeText(entry, "source");
        if (source.empty()) {
            source = feedTitle;
        }
        articles.push_back(NewsArticle{
            truncateUtf8(std::move(headline), kMaximumHeadlineBytes),
            truncateUtf8(std::move(source), 120), *publishedAt, std::move(url),
        });
    }
    return articles;
}

std::string normalizedHeadline(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    bool pendingSpace = false;
    for (const unsigned char character : value) {
        if (std::isspace(character)) {
            pendingSpace = !normalized.empty();
            continue;
        }
        if (pendingSpace) {
            normalized.push_back(' ');
            pendingSpace = false;
        }
        normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    return normalized;
}

std::uint64_t fnv1a(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string feedCacheKey(const NewsFeed& feed) {
    std::ostringstream key;
    key << "rss-feed-v1-" << std::hex << fnv1a(feed.url);
    return key.str();
}

bool freshCache(const ProviderCacheEntry& entry, std::chrono::seconds ttl,
                Clock::time_point now) {
    const auto age = now - entry.fetchedAt;
    return age >= Clock::duration::zero() && age <= ttl;
}

std::string fetchFeed(const HttpClientInterface& http, const NewsFeed& feed) {
    const auto response = http.request(HttpRequest{
        "GET", feed.url,
        {"Accept: application/rss+xml, application/atom+xml, application/xml, text/xml",
         "User-Agent: CppTradingBot/1.0 RSS reader"},
        {},
    });
    if (!response.isSuccess() || response.body.size() > kMaximumFeedBytes) {
        throw AppError(ErrorCode::Network, "RSS publisher did not return a valid feed");
    }
    return response.body;
}

std::string formatUtc(Clock::time_point timestamp) {
    const auto time = Clock::to_time_t(timestamp);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%d %H:%M UTC");
    return output.str();
}

} // namespace

std::vector<NewsFeed> defaultFinancialNewsFeeds() {
    return {
        {"CoinDesk", "https://www.coindesk.com/arc/outboundfeeds/rss/"},
        {"Federal Reserve Board - Press Releases", "https://www.federalreserve.gov/feeds/press_all.xml"},
        {"Federal Reserve Board - Monetary Policy", "https://www.federalreserve.gov/feeds/press_monetary.xml"},
        {"Federal Reserve Board - Meetings", "https://www.federalreserve.gov/feeds/boardmeetings.xml"},
        {"Federal Reserve Board - Speeches and Testimony",
         "https://www.federalreserve.gov/feeds/speeches_and_testimony.xml"},
        {"U.S. Bureau of Labor Statistics", "https://www.bls.gov/feed/bls_latest.rss"},
    };
}

RssFinancialNewsService::RssFinancialNewsService(
    Database& database, const HttpClientInterface& httpClient, std::vector<NewsFeed> feeds,
    std::chrono::seconds cacheTtl, std::chrono::hours maximumArticleAge)
    : database_(&database), httpClient_(&httpClient), feeds_(std::move(feeds)),
      cacheTtl_(cacheTtl), maximumArticleAge_(maximumArticleAge) {
    if (feeds_.empty() || feeds_.size() > 8 || cacheTtl_ < std::chrono::seconds::zero() ||
        maximumArticleAge_ <= std::chrono::hours::zero()) {
        throw AppError(ErrorCode::Configuration, "RSS feed settings are invalid");
    }
    for (const auto& feed : feeds_) {
        if (feed.source.empty() || !validHttpsUrl(feed.url)) {
            throw AppError(ErrorCode::Configuration,
                           "RSS feeds must have a name and a valid HTTPS URL");
        }
    }
}

NewsSnapshot RssFinancialNewsService::getLatestNews(
    std::size_t limit, Clock::time_point asOf) {
    std::lock_guard lock(requestMutex_);
    NewsSnapshot snapshot;
    snapshot.fetchedAt = asOf;
    if (limit == 0) {
        return snapshot;
    }
    limit = std::min<std::size_t>(limit, 20);
    std::vector<NewsArticle> gathered;

    for (const auto& feed : feeds_) {
        const auto cacheKey = feedCacheKey(feed);
        const auto cache = database_->loadProviderCache(cacheKey);
        const auto consume = [&](std::string_view payload, bool delayed, bool saveCache) {
            auto articles = parseFeed(payload, feed, asOf, maximumArticleAge_);
            if (saveCache) {
                database_->saveProviderCache(cacheKey, payload, asOf);
            }
            gathered.insert(gathered.end(),
                            std::make_move_iterator(articles.begin()),
                            std::make_move_iterator(articles.end()));
            if (delayed) {
                snapshot.delayedSources.push_back(feed.source);
            }
        };

        if (cache && freshCache(*cache, cacheTtl_, asOf)) {
            try {
                consume(cache->payload, false, false);
                continue;
            } catch (const std::exception&) {
                // An invalid cached document is replaced from the publisher below.
            }
        }

        try {
            const auto payload = fetchFeed(*httpClient_, feed);
            consume(payload, false, true);
            continue;
        } catch (const std::exception&) {
            // Use a valid stale cache as a fallback if the publisher is unavailable.
        }
        if (cache) {
            try {
                consume(cache->payload, true, false);
                continue;
            } catch (const std::exception&) {
                // If both the new feed and stale cache are invalid, report unavailable.
            }
        }
        snapshot.unavailableSources.push_back(feed.source);
    }

    std::stable_sort(gathered.begin(), gathered.end(), [](const NewsArticle& left, const NewsArticle& right) {
        return left.publishedAt > right.publishedAt;
    });
    std::unordered_set<std::string> seenHeadlines;
    std::unordered_set<std::string> seenUrls;
    for (auto& article : gathered) {
        const auto normalized = normalizedHeadline(article.headline);
        if (normalized.empty() || !seenHeadlines.insert(normalized).second ||
            !seenUrls.insert(article.url).second) {
            continue;
        }
        snapshot.articles.push_back(std::move(article));
        if (snapshot.articles.size() >= limit) {
            break;
        }
    }
    return snapshot;
}

std::string formatNewsMessage(const NewsSnapshot& snapshot, std::size_t maxArticles,
                              std::size_t maxCharacters) {
    maxCharacters = std::clamp<std::size_t>(maxCharacters, 200, 3500);
    std::string output = "Financial news from publisher RSS feeds | checked " +
                         formatUtc(snapshot.fetchedAt) + "\n";
    if (snapshot.articles.empty()) {
        output += "NEWS DATA UNAVAILABLE: no current, dated articles were returned.\n";
    } else {
        const auto count = std::min(maxArticles, snapshot.articles.size());
        for (std::size_t index = 0; index < count; ++index) {
            const auto& article = snapshot.articles[index];
            std::ostringstream entry;
            entry << '\n' << (index + 1) << ". " << article.headline << '\n'
                  << "Source: " << article.source << " | Published: "
                  << formatUtc(article.publishedAt) << '\n'
                  << article.url << '\n';
            if (output.size() + entry.str().size() > maxCharacters - 150) {
                break;
            }
            output += entry.str();
        }
    }
    for (const auto& source : snapshot.delayedSources) {
        const auto note = "DATA DELAYED: using a cached feed for " + source + ".\n";
        if (output.size() + note.size() <= maxCharacters - 100) {
            output += note;
        }
    }
    for (const auto& source : snapshot.unavailableSources) {
        const auto note = "Feed unavailable: " + source + ".\n";
        if (output.size() + note.size() <= maxCharacters - 100) {
            output += note;
        }
    }
    constexpr std::string_view footer =
        "News entries are publisher headlines and links; article claims are not independently verified.\n";
    if (output.size() + footer.size() <= maxCharacters) {
        output += footer;
    }
    return output;
}

} // namespace trading_bot
