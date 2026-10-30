#pragma once

#include "trading_bot/http_client.hpp"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace trading_bot {

class Database;

struct NewsFeed {
    std::string source;
    std::string url;
};

struct NewsArticle {
    std::string headline;
    std::string source;
    std::chrono::system_clock::time_point publishedAt;
    std::string url;
    // RSS guid / Atom id when the publisher supplies one. Kept as data only.
    std::string id;
    // Publisher update time, distinct from the original publication time.
    std::optional<std::chrono::system_clock::time_point> updatedAt;
};

enum class NewsImpact { High, Medium, Low };

[[nodiscard]] NewsImpact classifyGoldNews(const NewsArticle& article);
[[nodiscard]] bool isGoldRelevantNews(const NewsArticle& article);
[[nodiscard]] std::string newsImpactName(NewsImpact impact);

struct NewsSnapshot {
    std::vector<NewsArticle> articles;
    std::chrono::system_clock::time_point fetchedAt;
    std::vector<std::string> unavailableSources;
    std::vector<std::string> delayedSources;
};

[[nodiscard]] std::vector<NewsFeed> defaultFinancialNewsFeeds();

class FinancialNewsService {
public:
    virtual ~FinancialNewsService() = default;
    [[nodiscard]] virtual NewsSnapshot getLatestNews(
        std::size_t limit = 6,
        std::chrono::system_clock::time_point asOf = std::chrono::system_clock::now()) = 0;
};

// Reads only publisher RSS/Atom metadata (headline, source, date, URL). It does
// not fetch article pages or republish article bodies.
class RssFinancialNewsService final : public FinancialNewsService {
public:
    RssFinancialNewsService(Database& database, const HttpClientInterface& httpClient,
                            std::vector<NewsFeed> feeds = defaultFinancialNewsFeeds(),
                            std::chrono::seconds cacheTtl = std::chrono::minutes(5),
                            std::chrono::hours maximumArticleAge = std::chrono::hours(72));

    [[nodiscard]] NewsSnapshot getLatestNews(
        std::size_t limit = 6,
        std::chrono::system_clock::time_point asOf = std::chrono::system_clock::now()) override;

private:
    Database* database_;
    const HttpClientInterface* httpClient_;
    std::vector<NewsFeed> feeds_;
    std::chrono::seconds cacheTtl_;
    std::chrono::hours maximumArticleAge_;
    std::mutex requestMutex_;
};

[[nodiscard]] std::string formatNewsMessage(const NewsSnapshot& snapshot,
                                            std::size_t maxArticles = 3,
                                            std::size_t maxCharacters = 2000);

} // namespace trading_bot
