#include "trading_bot/database.hpp"

#include "trading_bot/error.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <system_error>

namespace trading_bot {
namespace {

void checkSqlite(int result, sqlite3* database, const char* operation) {
    if (result != SQLITE_OK) {
        const std::string details = database != nullptr ? sqlite3_errmsg(database) : sqlite3_errstr(result);
        throw AppError(ErrorCode::Storage, std::string(operation) + ": " + details);
    }
}

std::int64_t toEpochMilliseconds(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

std::chrono::system_clock::time_point fromEpochMilliseconds(std::int64_t value) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(value));
}

std::string columnText(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value != nullptr ? reinterpret_cast<const char*>(value) : std::string{};
}

void bindText(sqlite3_stmt* statement, int index, std::string_view value,
              sqlite3* database, const char* operation) {
    const std::string copy(value);
    checkSqlite(sqlite3_bind_text(statement, index, copy.c_str(), -1, SQLITE_TRANSIENT),
                database, operation);
}

std::int64_t insertAlert(sqlite3* database, std::string_view dedupeKey,
                         std::string_view category, std::string_view message,
                         std::string_view sourceUrl,
                         std::chrono::system_clock::time_point createdAt,
                         bool& inserted) {
    constexpr const char* sql =
        "INSERT OR IGNORE INTO scheduler_alerts "
        "(dedupe_key, category, message, source_url, created_at_ms) VALUES (?1, ?2, ?3, ?4, ?5)";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> statement(rawStatement, finalize);
    checkSqlite(prepareResult, database, "Unable to prepare alert insert");
    bindText(statement.get(), 1, dedupeKey, database, "Unable to bind alert dedupe key");
    bindText(statement.get(), 2, category, database, "Unable to bind alert category");
    bindText(statement.get(), 3, message, database, "Unable to bind alert message");
    bindText(statement.get(), 4, sourceUrl, database, "Unable to bind alert source URL");
    checkSqlite(sqlite3_bind_int64(statement.get(), 5, toEpochMilliseconds(createdAt)),
                database, "Unable to bind alert timestamp");
    const int stepResult = sqlite3_step(statement.get());
    if (stepResult != SQLITE_DONE) {
        checkSqlite(stepResult, database, "Unable to insert alert");
    }
    inserted = sqlite3_changes(database) == 1;
    if (inserted) {
        return sqlite3_last_insert_rowid(database);
    }

    sqlite3_stmt* rawQuery = nullptr;
    const int queryPrepare = sqlite3_prepare_v2(
        database, "SELECT id FROM scheduler_alerts WHERE dedupe_key = ?1", -1,
        &rawQuery, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> query(rawQuery, finalize);
    checkSqlite(queryPrepare, database, "Unable to prepare alert lookup");
    bindText(query.get(), 1, dedupeKey, database, "Unable to bind alert lookup key");
    const int queryStep = sqlite3_step(query.get());
    if (queryStep != SQLITE_ROW) {
        checkSqlite(queryStep, database, "Unable to find existing alert");
    }
    return sqlite3_column_int64(query.get(), 0);
}

void insertDeliveries(sqlite3* database, std::int64_t alertId,
                      const std::vector<std::int64_t>& userIds,
                      std::chrono::system_clock::time_point createdAt) {
    constexpr const char* sql =
        "INSERT OR IGNORE INTO scheduler_alert_deliveries "
        "(alert_id, user_id, state, attempts, next_attempt_ms) VALUES (?1, ?2, 'PENDING', 0, ?3)";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> statement(rawStatement, finalize);
    checkSqlite(prepareResult, database, "Unable to prepare alert delivery insert");
    for (const auto userId : userIds) {
        if (userId <= 0) {
            throw AppError(ErrorCode::Configuration, "Alert recipients must be positive Telegram user IDs");
        }
        sqlite3_reset(statement.get());
        sqlite3_clear_bindings(statement.get());
        checkSqlite(sqlite3_bind_int64(statement.get(), 1, alertId), database,
                    "Unable to bind alert delivery ID");
        checkSqlite(sqlite3_bind_int64(statement.get(), 2, userId), database,
                    "Unable to bind alert recipient");
        checkSqlite(sqlite3_bind_int64(statement.get(), 3, toEpochMilliseconds(createdAt)),
                    database, "Unable to bind alert delivery timestamp");
        const int stepResult = sqlite3_step(statement.get());
        if (stepResult != SQLITE_DONE) {
            checkSqlite(stepResult, database, "Unable to insert alert delivery");
        }
    }
}

bool safeHistoryLabel(std::string_view value) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' ||
               character == '-' || character == '.';
    });
}

} // namespace

Database::Database(const std::filesystem::path& path) {
    if (path != ":memory:" && !path.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            throw AppError(ErrorCode::Storage,
                           "Unable to create database directory: " + error.message());
        }
    }

    const auto pathString = path.string();
    const int result = sqlite3_open_v2(pathString.c_str(), &handle_,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                       nullptr);
    if (result != SQLITE_OK) {
        const std::string details = handle_ != nullptr ? sqlite3_errmsg(handle_) : sqlite3_errstr(result);
        if (handle_ != nullptr) {
            sqlite3_close(handle_);
            handle_ = nullptr;
        }
        throw AppError(ErrorCode::Storage, "Unable to open database: " + details);
    }

    if (path != ":memory:") {
        std::error_code permissionError;
        std::filesystem::permissions(path,
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace,
                                     permissionError);
        if (permissionError) {
            sqlite3_close(handle_);
            handle_ = nullptr;
            throw AppError(ErrorCode::Storage,
                           "Unable to restrict SQLite database permissions to the current user");
        }
    }

    try {
        execute("PRAGMA foreign_keys = ON");
        execute("CREATE TABLE IF NOT EXISTS app_metadata ("
                "key TEXT PRIMARY KEY NOT NULL, "
                "value TEXT NOT NULL, "
                "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
        execute("CREATE TABLE IF NOT EXISTS provider_cache ("
                "cache_key TEXT PRIMARY KEY NOT NULL, "
                "payload TEXT NOT NULL, "
                "fetched_at_ms INTEGER NOT NULL)");
        execute("CREATE TABLE IF NOT EXISTS ai_summary_cache ("
                "cache_key TEXT PRIMARY KEY NOT NULL, "
                "summary TEXT NOT NULL, "
                "generated_at_ms INTEGER NOT NULL)");
        execute("CREATE TABLE IF NOT EXISTS market_candles ("
                "cache_key TEXT NOT NULL, "
                "symbol TEXT NOT NULL, "
                "source TEXT NOT NULL, "
                "interval TEXT NOT NULL, "
                "start_ms INTEGER NOT NULL, "
                "end_ms INTEGER NOT NULL, "
                "open REAL NOT NULL, high REAL NOT NULL, low REAL NOT NULL, close REAL NOT NULL, "
                "fetched_at_ms INTEGER NOT NULL, "
                "PRIMARY KEY(cache_key, start_ms, end_ms))");
        execute("CREATE TABLE IF NOT EXISTS signal_scenarios ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                "market TEXT NOT NULL, analysis_at_ms INTEGER NOT NULL, timeframe TEXT NOT NULL, "
                "trend TEXT NOT NULL, signal_state TEXT NOT NULL, data_status TEXT NOT NULL, "
                "quote_price REAL, latest_candle_close REAL, sma_20 REAL, ema_fast REAL, ema_slow REAL, "
                "rsi_14 REAL, macd_histogram REAL, atr_14 REAL, "
                "entry_low REAL, entry_high REAL, stop_loss REAL, "
                "take_profit_1 REAL, take_profit_2 REAL, risk_reward_1 REAL, risk_reward_2 REAL, "
                "data_source TEXT NOT NULL, quote_timestamp_ms INTEGER, candle_timestamp_ms INTEGER, "
                "reasons_json TEXT NOT NULL, conflicts_json TEXT NOT NULL)");
        execute("CREATE TABLE IF NOT EXISTS backtest_runs ("
                "run_key TEXT PRIMARY KEY NOT NULL, market TEXT NOT NULL, source TEXT NOT NULL, "
                "timeframe TEXT NOT NULL, status TEXT NOT NULL, reason TEXT NOT NULL, "
                "history_start_ms INTEGER NOT NULL, history_end_ms INTEGER NOT NULL, "
                "candle_count INTEGER NOT NULL, decision_bars INTEGER NOT NULL, "
                "starting_balance REAL NOT NULL, ending_balance REAL NOT NULL, net_pnl REAL NOT NULL, "
                "total_fees REAL NOT NULL, fee_bps REAL NOT NULL, slippage_bps REAL NOT NULL, "
                "risk_fraction REAL NOT NULL, trade_count INTEGER NOT NULL, wins INTEGER NOT NULL, "
                "losses INTEGER NOT NULL, win_rate REAL, average_realized_r REAL, "
                "maximum_drawdown REAL NOT NULL, maximum_drawdown_percent REAL NOT NULL, "
                "created_at_ms INTEGER NOT NULL)");
        execute("CREATE TABLE IF NOT EXISTS backtest_trades ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, run_key TEXT NOT NULL, sequence INTEGER NOT NULL, "
                "market TEXT NOT NULL, source TEXT NOT NULL, timeframe TEXT NOT NULL, "
                "direction TEXT NOT NULL, entry_at_ms INTEGER NOT NULL, exit_at_ms INTEGER NOT NULL, "
                "entry_price REAL NOT NULL, average_exit_price REAL NOT NULL, stop_loss REAL NOT NULL, "
                "target1 REAL NOT NULL, target2 REAL NOT NULL, quantity REAL NOT NULL, "
                "gross_pnl REAL NOT NULL, fees_paid REAL NOT NULL, net_pnl REAL NOT NULL, "
                "risk_amount REAL NOT NULL, realized_r REAL NOT NULL, target1_reached INTEGER NOT NULL, "
                "exit_reason TEXT NOT NULL, UNIQUE(run_key, sequence), "
                "FOREIGN KEY(run_key) REFERENCES backtest_runs(run_key))");
        execute("CREATE TABLE IF NOT EXISTS scheduler_alerts ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, dedupe_key TEXT NOT NULL UNIQUE, "
                "category TEXT NOT NULL, message TEXT NOT NULL, source_url TEXT NOT NULL, "
                "created_at_ms INTEGER NOT NULL)");
        execute("CREATE TABLE IF NOT EXISTS scheduler_alert_deliveries ("
                "alert_id INTEGER NOT NULL, user_id INTEGER NOT NULL, state TEXT NOT NULL DEFAULT 'PENDING', "
                "attempts INTEGER NOT NULL DEFAULT 0, next_attempt_ms INTEGER NOT NULL, "
                "delivered_at_ms INTEGER, last_error_category TEXT NOT NULL DEFAULT '', "
                "PRIMARY KEY(alert_id, user_id), "
                "FOREIGN KEY(alert_id) REFERENCES scheduler_alerts(id) ON DELETE CASCADE)");
        execute("CREATE INDEX IF NOT EXISTS scheduler_delivery_due_idx "
                "ON scheduler_alert_deliveries(state, next_attempt_ms)");
        execute("CREATE TABLE IF NOT EXISTS scheduler_errors ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, component TEXT NOT NULL, "
                "error_category TEXT NOT NULL, occurred_at_ms INTEGER NOT NULL)");
        execute("CREATE TABLE IF NOT EXISTS monitor_signal_state ("
                "market TEXT PRIMARY KEY NOT NULL, state TEXT NOT NULL, updated_at_ms INTEGER NOT NULL)");
    } catch (...) {
        sqlite3_close(handle_);
        handle_ = nullptr;
        throw;
    }
}

Database::~Database() {
    if (handle_ != nullptr) {
        sqlite3_close(handle_);
    }
}

Database::Database(Database&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) {
            sqlite3_close(handle_);
        }
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

void Database::execute(std::string_view sql) {
    std::lock_guard lock(mutex_);
    char* errorMessage = nullptr;
    const std::string statement(sql);
    const int result = sqlite3_exec(handle_, statement.c_str(), nullptr, nullptr, &errorMessage);
    if (result != SQLITE_OK) {
        const std::string details = errorMessage != nullptr ? errorMessage : sqlite3_errmsg(handle_);
        sqlite3_free(errorMessage);
        throw AppError(ErrorCode::Storage, "SQLite statement failed: " + details);
    }
}

std::int64_t Database::queryInt64(std::string_view sql) const {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* rawStatement = nullptr;
    const std::string statement(sql);
    const int prepareResult = sqlite3_prepare_v2(handle_, statement.c_str(), -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> prepared(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare SQLite query");

    const int stepResult = sqlite3_step(prepared.get());
    if (stepResult == SQLITE_DONE) {
        throw AppError(ErrorCode::Storage, "SQLite query returned no rows");
    }
    if (stepResult != SQLITE_ROW) {
        checkSqlite(stepResult, handle_, "Unable to execute SQLite query");
    }
    return sqlite3_column_int64(prepared.get(), 0);
}

std::optional<std::int64_t> Database::metadataInt64(std::string_view key) const {
    std::lock_guard lock(mutex_);
    constexpr const char* sql = "SELECT CAST(value AS INTEGER) FROM app_metadata WHERE key = ?1";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(handle_, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> prepared(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare metadata query");

    const std::string keyString(key);
    checkSqlite(sqlite3_bind_text(prepared.get(), 1, keyString.c_str(), -1, SQLITE_TRANSIENT),
                handle_, "Unable to bind metadata key");
    const int stepResult = sqlite3_step(prepared.get());
    if (stepResult == SQLITE_DONE) {
        return std::nullopt;
    }
    if (stepResult != SQLITE_ROW) {
        checkSqlite(stepResult, handle_, "Unable to read metadata");
    }
    return sqlite3_column_int64(prepared.get(), 0);
}

std::optional<std::string> Database::metadataText(std::string_view key) const {
    std::lock_guard lock(mutex_);
    constexpr const char* sql = "SELECT value FROM app_metadata WHERE key = ?1";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(handle_, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> prepared(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare metadata query");
    const std::string keyString(key);
    checkSqlite(sqlite3_bind_text(prepared.get(), 1, keyString.c_str(), -1, SQLITE_TRANSIENT),
                handle_, "Unable to bind metadata key");
    const int stepResult = sqlite3_step(prepared.get());
    if (stepResult == SQLITE_DONE) {
        return std::nullopt;
    }
    if (stepResult != SQLITE_ROW) {
        checkSqlite(stepResult, handle_, "Unable to read metadata");
    }
    return columnText(prepared.get(), 0);
}

void Database::setMetadataText(std::string_view key, std::string_view value) {
    std::lock_guard lock(mutex_);
    constexpr const char* sql =
        "INSERT INTO app_metadata (key, value) VALUES (?1, ?2) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = CURRENT_TIMESTAMP";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(handle_, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> prepared(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare metadata update");
    const std::string keyString(key);
    const std::string valueString(value);
    checkSqlite(sqlite3_bind_text(prepared.get(), 1, keyString.c_str(), -1, SQLITE_TRANSIENT),
                handle_, "Unable to bind metadata key");
    checkSqlite(sqlite3_bind_text(prepared.get(), 2, valueString.c_str(), -1, SQLITE_TRANSIENT),
                handle_, "Unable to bind metadata value");
    const int stepResult = sqlite3_step(prepared.get());
    if (stepResult != SQLITE_DONE) {
        checkSqlite(stepResult, handle_, "Unable to update metadata");
    }
}

void Database::setMetadataInt64(std::string_view key, std::int64_t value) {
    std::lock_guard lock(mutex_);
    constexpr const char* sql =
        "INSERT INTO app_metadata (key, value) VALUES (?1, ?2) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = CURRENT_TIMESTAMP";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(handle_, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> prepared(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare metadata update");

    const std::string keyString(key);
    checkSqlite(sqlite3_bind_text(prepared.get(), 1, keyString.c_str(), -1, SQLITE_TRANSIENT),
                handle_, "Unable to bind metadata key");
    checkSqlite(sqlite3_bind_int64(prepared.get(), 2, value),
                handle_, "Unable to bind metadata value");
    const int stepResult = sqlite3_step(prepared.get());
    if (stepResult != SQLITE_DONE) {
        checkSqlite(stepResult, handle_, "Unable to update metadata");
    }
}

std::optional<ProviderCacheEntry> Database::loadProviderCache(std::string_view key) const {
    std::lock_guard lock(mutex_);
    constexpr const char* sql = "SELECT payload, fetched_at_ms FROM provider_cache WHERE cache_key = ?1";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(handle_, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> prepared(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare provider cache query");

    const std::string keyString(key);
    checkSqlite(sqlite3_bind_text(prepared.get(), 1, keyString.c_str(), -1, SQLITE_TRANSIENT),
                handle_, "Unable to bind provider cache key");
    const int stepResult = sqlite3_step(prepared.get());
    if (stepResult == SQLITE_DONE) {
        return std::nullopt;
    }
    if (stepResult != SQLITE_ROW) {
        checkSqlite(stepResult, handle_, "Unable to read provider cache");
    }
    return ProviderCacheEntry{
        columnText(prepared.get(), 0),
        fromEpochMilliseconds(sqlite3_column_int64(prepared.get(), 1)),
    };
}

void Database::saveProviderCache(std::string_view key, std::string_view payload,
                                 std::chrono::system_clock::time_point fetchedAt) {
    std::lock_guard lock(mutex_);
    constexpr const char* sql =
        "INSERT INTO provider_cache (cache_key, payload, fetched_at_ms) VALUES (?1, ?2, ?3) "
        "ON CONFLICT(cache_key) DO UPDATE SET payload = excluded.payload, "
        "fetched_at_ms = excluded.fetched_at_ms";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(handle_, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> prepared(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare provider cache update");

    const std::string keyString(key);
    const std::string payloadString(payload);
    checkSqlite(sqlite3_bind_text(prepared.get(), 1, keyString.c_str(), -1, SQLITE_TRANSIENT),
                handle_, "Unable to bind provider cache key");
    checkSqlite(sqlite3_bind_text(prepared.get(), 2, payloadString.c_str(), -1, SQLITE_TRANSIENT),
                handle_, "Unable to bind provider payload");
    checkSqlite(sqlite3_bind_int64(prepared.get(), 3, toEpochMilliseconds(fetchedAt)),
                handle_, "Unable to bind provider cache timestamp");
    const int stepResult = sqlite3_step(prepared.get());
    if (stepResult != SQLITE_DONE) {
        checkSqlite(stepResult, handle_, "Unable to update provider cache");
    }
}

std::optional<AiSummaryCacheEntry> Database::loadAiSummary(std::string_view key) const {
    std::lock_guard lock(mutex_);
    constexpr const char* sql =
        "SELECT summary, generated_at_ms FROM ai_summary_cache WHERE cache_key = ?1";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(handle_, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> prepared(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare AI summary cache query");

    const std::string keyString(key);
    checkSqlite(sqlite3_bind_text(prepared.get(), 1, keyString.c_str(), -1, SQLITE_TRANSIENT),
                handle_, "Unable to bind AI summary cache key");
    const int stepResult = sqlite3_step(prepared.get());
    if (stepResult == SQLITE_DONE) {
        return std::nullopt;
    }
    if (stepResult != SQLITE_ROW) {
        checkSqlite(stepResult, handle_, "Unable to read AI summary cache");
    }
    return AiSummaryCacheEntry{
        columnText(prepared.get(), 0),
        fromEpochMilliseconds(sqlite3_column_int64(prepared.get(), 1)),
    };
}

void Database::saveAiSummary(std::string_view key, std::string_view summary,
                             std::chrono::system_clock::time_point generatedAt) {
    std::lock_guard lock(mutex_);
    constexpr const char* sql =
        "INSERT INTO ai_summary_cache (cache_key, summary, generated_at_ms) VALUES (?1, ?2, ?3) "
        "ON CONFLICT(cache_key) DO UPDATE SET summary = excluded.summary, "
        "generated_at_ms = excluded.generated_at_ms";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(handle_, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> prepared(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare AI summary cache update");

    const std::string keyString(key);
    const std::string summaryString(summary);
    checkSqlite(sqlite3_bind_text(prepared.get(), 1, keyString.c_str(), -1, SQLITE_TRANSIENT),
                handle_, "Unable to bind AI summary cache key");
    checkSqlite(sqlite3_bind_text(prepared.get(), 2, summaryString.c_str(), -1, SQLITE_TRANSIENT),
                handle_, "Unable to bind AI summary");
    checkSqlite(sqlite3_bind_int64(prepared.get(), 3, toEpochMilliseconds(generatedAt)),
                handle_, "Unable to bind AI summary timestamp");
    const int stepResult = sqlite3_step(prepared.get());
    if (stepResult != SQLITE_DONE) {
        checkSqlite(stepResult, handle_, "Unable to update AI summary cache");
    }

    constexpr std::int64_t kRetentionMilliseconds = 30LL * 24 * 60 * 60 * 1000;
    const auto cutoff = toEpochMilliseconds(generatedAt) - kRetentionMilliseconds;
    constexpr const char* pruneSql = "DELETE FROM ai_summary_cache WHERE generated_at_ms < ?1";
    sqlite3_stmt* rawPrune = nullptr;
    const int prunePrepare = sqlite3_prepare_v2(handle_, pruneSql, -1, &rawPrune, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> pruneStatement(rawPrune, finalize);
    checkSqlite(prunePrepare, handle_, "Unable to prepare AI summary cache cleanup");
    checkSqlite(sqlite3_bind_int64(pruneStatement.get(), 1, cutoff),
                handle_, "Unable to bind AI summary cleanup timestamp");
    const int pruneStep = sqlite3_step(pruneStatement.get());
    if (pruneStep != SQLITE_DONE) {
        checkSqlite(pruneStep, handle_, "Unable to clean old AI summaries");
    }
}

void Database::saveSignalScenario(const SignalScenario& scenario) {
    std::lock_guard lock(mutex_);
    constexpr const char* sql =
        "INSERT INTO signal_scenarios (market, analysis_at_ms, timeframe, trend, signal_state, "
        "data_status, quote_price, latest_candle_close, sma_20, ema_fast, ema_slow, rsi_14, "
        "macd_histogram, atr_14, entry_low, entry_high, stop_loss, take_profit_1, take_profit_2, "
        "risk_reward_1, risk_reward_2, data_source, quote_timestamp_ms, candle_timestamp_ms, "
        "reasons_json, conflicts_json) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, "
        "?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22, ?23, ?24, ?25, ?26)";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(handle_, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> prepared(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare signal scenario insert");

    const std::string market = marketSymbolName(scenario.market);
    const std::string trend = trendClassificationName(scenario.trend);
    const std::string state = signalStateName(scenario.state);
    const std::string status = signalDataStatusName(scenario.dataStatus);
    const std::string reasons = nlohmann::json(scenario.reasons).dump();
    const std::string conflicts = nlohmann::json(scenario.conflicts).dump();
    const auto bindText = [this, &prepared](int index, const std::string& value, const char* operation) {
        checkSqlite(sqlite3_bind_text(prepared.get(), index, value.c_str(), -1, SQLITE_TRANSIENT),
                    handle_, operation);
    };
    const auto bindDouble = [this, &prepared](int index, const std::optional<double>& value,
                                              const char* operation) {
        const int result = value ? sqlite3_bind_double(prepared.get(), index, *value)
                                 : sqlite3_bind_null(prepared.get(), index);
        checkSqlite(result, handle_, operation);
    };
    const auto bindTimestamp = [this, &prepared](int index,
                                                  const std::optional<std::chrono::system_clock::time_point>& value,
                                                  const char* operation) {
        const int result = value
            ? sqlite3_bind_int64(prepared.get(), index, toEpochMilliseconds(*value))
            : sqlite3_bind_null(prepared.get(), index);
        checkSqlite(result, handle_, operation);
    };

    bindText(1, market, "Unable to bind scenario market");
    checkSqlite(sqlite3_bind_int64(prepared.get(), 2, toEpochMilliseconds(scenario.analysisAt)),
                handle_, "Unable to bind scenario analysis timestamp");
    bindText(3, scenario.timeframe, "Unable to bind scenario timeframe");
    bindText(4, trend, "Unable to bind scenario trend");
    bindText(5, state, "Unable to bind scenario state");
    bindText(6, status, "Unable to bind scenario data status");
    bindDouble(7, scenario.quotePrice, "Unable to bind scenario quote price");
    bindDouble(8, scenario.latestCandleClose, "Unable to bind scenario candle close");
    bindDouble(9, scenario.sma20, "Unable to bind scenario SMA");
    bindDouble(10, scenario.emaFast, "Unable to bind scenario fast EMA");
    bindDouble(11, scenario.emaSlow, "Unable to bind scenario slow EMA");
    bindDouble(12, scenario.rsi14, "Unable to bind scenario RSI");
    bindDouble(13, scenario.macdHistogram, "Unable to bind scenario MACD histogram");
    bindDouble(14, scenario.atr, "Unable to bind scenario ATR");
    bindDouble(15, scenario.entryZoneLow, "Unable to bind scenario entry low");
    bindDouble(16, scenario.entryZoneHigh, "Unable to bind scenario entry high");
    bindDouble(17, scenario.stopLoss, "Unable to bind scenario stop loss");
    bindDouble(18, scenario.takeProfit1, "Unable to bind scenario TP1");
    bindDouble(19, scenario.takeProfit2, "Unable to bind scenario TP2");
    bindDouble(20, scenario.riskReward1, "Unable to bind scenario TP1 risk/reward");
    bindDouble(21, scenario.riskReward2, "Unable to bind scenario TP2 risk/reward");
    bindText(22, scenario.dataSource, "Unable to bind scenario source");
    bindTimestamp(23, scenario.quoteTimestamp, "Unable to bind scenario quote timestamp");
    bindTimestamp(24, scenario.candleTimestamp, "Unable to bind scenario candle timestamp");
    bindText(25, reasons, "Unable to bind scenario reasons");
    bindText(26, conflicts, "Unable to bind scenario conflicts");

    const int stepResult = sqlite3_step(prepared.get());
    if (stepResult != SQLITE_DONE) {
        checkSqlite(stepResult, handle_, "Unable to store signal scenario");
    }
}

void Database::saveBacktestResult(const BacktestResult& result) {
    std::lock_guard lock(mutex_);
    if (result.runKey.empty()) {
        throw AppError(ErrorCode::Storage, "Backtest result has no stable run key");
    }
    execute("BEGIN IMMEDIATE");
    try {
        constexpr const char* runSql =
            "INSERT OR IGNORE INTO backtest_runs (run_key, market, source, timeframe, status, reason, "
            "history_start_ms, history_end_ms, candle_count, decision_bars, starting_balance, "
            "ending_balance, net_pnl, total_fees, fee_bps, slippage_bps, risk_fraction, trade_count, "
            "wins, losses, win_rate, average_realized_r, maximum_drawdown, "
            "maximum_drawdown_percent, created_at_ms) VALUES ("
            "?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, "
            "?18, ?19, ?20, ?21, ?22, ?23, ?24, ?25)";
        sqlite3_stmt* rawRun = nullptr;
        const int runPrepare = sqlite3_prepare_v2(handle_, runSql, -1, &rawRun, nullptr);
        const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
        std::unique_ptr<sqlite3_stmt, decltype(finalize)> runStatement(rawRun, finalize);
        checkSqlite(runPrepare, handle_, "Unable to prepare backtest summary insert");

        const std::string market = marketSymbolName(result.market);
        const std::string status = backtestStatusName(result.status);
        const auto bindRunText = [this, &runStatement](int index, const std::string& value,
                                                       const char* operation) {
            checkSqlite(sqlite3_bind_text(runStatement.get(), index, value.c_str(), -1, SQLITE_TRANSIENT),
                        handle_, operation);
        };
        bindRunText(1, result.runKey, "Unable to bind backtest run key");
        bindRunText(2, market, "Unable to bind backtest market");
        bindRunText(3, result.source, "Unable to bind backtest source");
        bindRunText(4, result.timeframe, "Unable to bind backtest timeframe");
        bindRunText(5, status, "Unable to bind backtest status");
        bindRunText(6, result.reason, "Unable to bind backtest reason");
        checkSqlite(sqlite3_bind_int64(runStatement.get(), 7, toEpochMilliseconds(result.historyStart)),
                    handle_, "Unable to bind backtest history start");
        checkSqlite(sqlite3_bind_int64(runStatement.get(), 8, toEpochMilliseconds(result.historyEnd)),
                    handle_, "Unable to bind backtest history end");
        checkSqlite(sqlite3_bind_int64(runStatement.get(), 9,
                                      static_cast<sqlite3_int64>(result.candleCount)),
                    handle_, "Unable to bind backtest candle count");
        checkSqlite(sqlite3_bind_int64(runStatement.get(), 10,
                                      static_cast<sqlite3_int64>(result.decisionBars)),
                    handle_, "Unable to bind backtest decision bar count");
        checkSqlite(sqlite3_bind_double(runStatement.get(), 11, result.settings.startingBalance),
                    handle_, "Unable to bind backtest starting balance");
        checkSqlite(sqlite3_bind_double(runStatement.get(), 12, result.endingBalance),
                    handle_, "Unable to bind backtest ending balance");
        checkSqlite(sqlite3_bind_double(runStatement.get(), 13, result.netPnl),
                    handle_, "Unable to bind backtest P&L");
        checkSqlite(sqlite3_bind_double(runStatement.get(), 14, result.totalFees),
                    handle_, "Unable to bind backtest fees");
        checkSqlite(sqlite3_bind_double(runStatement.get(), 15, result.settings.feeBasisPoints),
                    handle_, "Unable to bind backtest fee setting");
        checkSqlite(sqlite3_bind_double(runStatement.get(), 16, result.settings.slippageBasisPoints),
                    handle_, "Unable to bind backtest slippage setting");
        checkSqlite(sqlite3_bind_double(runStatement.get(), 17, result.settings.riskFraction),
                    handle_, "Unable to bind backtest risk setting");
        checkSqlite(sqlite3_bind_int64(runStatement.get(), 18,
                                      static_cast<sqlite3_int64>(result.trades.size())),
                    handle_, "Unable to bind backtest trade count");
        checkSqlite(sqlite3_bind_int64(runStatement.get(), 19,
                                      static_cast<sqlite3_int64>(result.wins)),
                    handle_, "Unable to bind backtest win count");
        checkSqlite(sqlite3_bind_int64(runStatement.get(), 20,
                                      static_cast<sqlite3_int64>(result.losses)),
                    handle_, "Unable to bind backtest loss count");
        const auto bindOptional = [this, &runStatement](int index,
                                                         const std::optional<double>& value,
                                                         const char* operation) {
            const int code = value ? sqlite3_bind_double(runStatement.get(), index, *value)
                                   : sqlite3_bind_null(runStatement.get(), index);
            checkSqlite(code, handle_, operation);
        };
        bindOptional(21, result.winRate, "Unable to bind backtest win rate");
        bindOptional(22, result.averageRiskReward, "Unable to bind average realized R");
        checkSqlite(sqlite3_bind_double(runStatement.get(), 23, result.maximumDrawdown),
                    handle_, "Unable to bind maximum backtest drawdown");
        checkSqlite(sqlite3_bind_double(runStatement.get(), 24, result.maximumDrawdownPercent),
                    handle_, "Unable to bind maximum backtest drawdown percentage");
        checkSqlite(sqlite3_bind_int64(runStatement.get(), 25,
                                      toEpochMilliseconds(std::chrono::system_clock::now())),
                    handle_, "Unable to bind backtest creation timestamp");
        const int runStep = sqlite3_step(runStatement.get());
        if (runStep != SQLITE_DONE) {
            checkSqlite(runStep, handle_, "Unable to store backtest summary");
        }
        const bool isNewRun = sqlite3_changes(handle_) > 0;
        runStatement.reset();
        if (!isNewRun) {
            execute("COMMIT");
            return;
        }

        constexpr const char* tradeSql =
            "INSERT INTO backtest_trades (run_key, sequence, market, source, timeframe, direction, "
            "entry_at_ms, exit_at_ms, entry_price, average_exit_price, stop_loss, target1, target2, "
            "quantity, gross_pnl, fees_paid, net_pnl, risk_amount, realized_r, target1_reached, "
            "exit_reason) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, "
            "?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21)";
        sqlite3_stmt* rawTrade = nullptr;
        const int tradePrepare = sqlite3_prepare_v2(handle_, tradeSql, -1, &rawTrade, nullptr);
        std::unique_ptr<sqlite3_stmt, decltype(finalize)> tradeStatement(rawTrade, finalize);
        checkSqlite(tradePrepare, handle_, "Unable to prepare simulated trade insert");
        for (std::size_t index = 0; index < result.trades.size(); ++index) {
            const auto& trade = result.trades[index];
            sqlite3_reset(tradeStatement.get());
            sqlite3_clear_bindings(tradeStatement.get());
            const std::string tradeMarket = marketSymbolName(trade.market);
            const std::string direction = simulatedDirectionName(trade.direction);
            const auto bindTradeText = [this, &tradeStatement](int parameter,
                                                                const std::string& value,
                                                                const char* operation) {
                checkSqlite(sqlite3_bind_text(tradeStatement.get(), parameter, value.c_str(), -1,
                                              SQLITE_TRANSIENT), handle_, operation);
            };
            bindTradeText(1, result.runKey, "Unable to bind trade run key");
            checkSqlite(sqlite3_bind_int64(tradeStatement.get(), 2,
                                           static_cast<sqlite3_int64>(index + 1)),
                        handle_, "Unable to bind trade sequence");
            bindTradeText(3, tradeMarket, "Unable to bind simulated trade market");
            bindTradeText(4, trade.source, "Unable to bind simulated trade source");
            bindTradeText(5, trade.timeframe, "Unable to bind simulated trade timeframe");
            bindTradeText(6, direction, "Unable to bind simulated trade direction");
            checkSqlite(sqlite3_bind_int64(tradeStatement.get(), 7, toEpochMilliseconds(trade.entryTime)),
                        handle_, "Unable to bind simulated entry time");
            checkSqlite(sqlite3_bind_int64(tradeStatement.get(), 8, toEpochMilliseconds(trade.exitTime)),
                        handle_, "Unable to bind simulated exit time");
            checkSqlite(sqlite3_bind_double(tradeStatement.get(), 9, trade.entryPrice),
                        handle_, "Unable to bind simulated entry price");
            checkSqlite(sqlite3_bind_double(tradeStatement.get(), 10, trade.averageExitPrice),
                        handle_, "Unable to bind simulated exit price");
            checkSqlite(sqlite3_bind_double(tradeStatement.get(), 11, trade.stopLoss),
                        handle_, "Unable to bind simulated stop loss");
            checkSqlite(sqlite3_bind_double(tradeStatement.get(), 12, trade.target1),
                        handle_, "Unable to bind simulated TP1");
            checkSqlite(sqlite3_bind_double(tradeStatement.get(), 13, trade.target2),
                        handle_, "Unable to bind simulated TP2");
            checkSqlite(sqlite3_bind_double(tradeStatement.get(), 14, trade.quantity),
                        handle_, "Unable to bind simulated quantity");
            checkSqlite(sqlite3_bind_double(tradeStatement.get(), 15, trade.grossPnl),
                        handle_, "Unable to bind simulated gross P&L");
            checkSqlite(sqlite3_bind_double(tradeStatement.get(), 16, trade.feesPaid),
                        handle_, "Unable to bind simulated fees");
            checkSqlite(sqlite3_bind_double(tradeStatement.get(), 17, trade.netPnl),
                        handle_, "Unable to bind simulated net P&L");
            checkSqlite(sqlite3_bind_double(tradeStatement.get(), 18, trade.riskAmount),
                        handle_, "Unable to bind simulated risk amount");
            checkSqlite(sqlite3_bind_double(tradeStatement.get(), 19, trade.realizedRiskReward),
                        handle_, "Unable to bind simulated realized R");
            checkSqlite(sqlite3_bind_int(tradeStatement.get(), 20, trade.target1Reached ? 1 : 0),
                        handle_, "Unable to bind simulated TP1 state");
            bindTradeText(21, trade.exitReason, "Unable to bind simulated exit reason");
            const int tradeStep = sqlite3_step(tradeStatement.get());
            if (tradeStep != SQLITE_DONE) {
                checkSqlite(tradeStep, handle_, "Unable to store simulated trade");
            }
        }
        execute("COMMIT");
    } catch (...) {
        try {
            execute("ROLLBACK");
        } catch (...) {
            // Preserve the original storage exception.
        }
        throw;
    }
}

std::vector<SimulatedTrade> Database::loadRecentBacktestTrades(std::size_t limit) const {
    std::lock_guard lock(mutex_);
    if (limit == 0) {
        return {};
    }
    limit = std::min<std::size_t>(limit, 100);
    constexpr const char* sql =
        "SELECT market, source, timeframe, direction, entry_at_ms, exit_at_ms, entry_price, "
        "average_exit_price, stop_loss, target1, target2, quantity, gross_pnl, fees_paid, net_pnl, "
        "risk_amount, realized_r, "
        "target1_reached, exit_reason FROM backtest_trades "
        "ORDER BY exit_at_ms DESC, id DESC LIMIT ?1";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(handle_, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> prepared(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare simulated history query");
    checkSqlite(sqlite3_bind_int64(prepared.get(), 1, static_cast<sqlite3_int64>(limit)),
                handle_, "Unable to bind simulated history limit");

    std::vector<SimulatedTrade> trades;
    while (true) {
        const int stepResult = sqlite3_step(prepared.get());
        if (stepResult == SQLITE_DONE) {
            break;
        }
        if (stepResult != SQLITE_ROW) {
            checkSqlite(stepResult, handle_, "Unable to read simulated history");
        }
        SimulatedTrade trade;
        const auto market = columnText(prepared.get(), 0);
        trade.market = market == "XAU/USD" ? MarketSymbol::XauUsd : MarketSymbol::BtcUsd;
        trade.source = columnText(prepared.get(), 1);
        trade.timeframe = columnText(prepared.get(), 2);
        trade.direction = columnText(prepared.get(), 3) == "SHORT"
                              ? SimulatedDirection::Short : SimulatedDirection::Long;
        trade.entryTime = fromEpochMilliseconds(sqlite3_column_int64(prepared.get(), 4));
        trade.exitTime = fromEpochMilliseconds(sqlite3_column_int64(prepared.get(), 5));
        trade.entryPrice = sqlite3_column_double(prepared.get(), 6);
        trade.averageExitPrice = sqlite3_column_double(prepared.get(), 7);
        trade.stopLoss = sqlite3_column_double(prepared.get(), 8);
        trade.target1 = sqlite3_column_double(prepared.get(), 9);
        trade.target2 = sqlite3_column_double(prepared.get(), 10);
        trade.quantity = sqlite3_column_double(prepared.get(), 11);
        trade.grossPnl = sqlite3_column_double(prepared.get(), 12);
        trade.feesPaid = sqlite3_column_double(prepared.get(), 13);
        trade.netPnl = sqlite3_column_double(prepared.get(), 14);
        trade.riskAmount = sqlite3_column_double(prepared.get(), 15);
        trade.realizedRiskReward = sqlite3_column_double(prepared.get(), 16);
        trade.target1Reached = sqlite3_column_int(prepared.get(), 17) != 0;
        trade.exitReason = columnText(prepared.get(), 18);
        trades.push_back(std::move(trade));
    }
    return trades;
}

void Database::replaceMarketCandles(std::string_view cacheKey,
                                    const std::vector<OhlcCandle>& candles) {
    std::lock_guard lock(mutex_);
    const std::string keyString(cacheKey);
    execute("BEGIN IMMEDIATE");
    try {
        sqlite3_stmt* rawDelete = nullptr;
        const int deletePrepare = sqlite3_prepare_v2(
            handle_, "DELETE FROM market_candles WHERE cache_key = ?1", -1, &rawDelete, nullptr);
        const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
        std::unique_ptr<sqlite3_stmt, decltype(finalize)> deleteStatement(rawDelete, finalize);
        checkSqlite(deletePrepare, handle_, "Unable to prepare market candle replacement");
        checkSqlite(sqlite3_bind_text(deleteStatement.get(), 1, keyString.c_str(), -1, SQLITE_TRANSIENT),
                    handle_, "Unable to bind market candle cache key");
        const int deleteStep = sqlite3_step(deleteStatement.get());
        if (deleteStep != SQLITE_DONE) {
            checkSqlite(deleteStep, handle_, "Unable to clear previous market candles");
        }

        constexpr const char* insertSql =
            "INSERT INTO market_candles (cache_key, symbol, source, interval, start_ms, end_ms, "
            "open, high, low, close, fetched_at_ms) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)";
        sqlite3_stmt* rawInsert = nullptr;
        const int insertPrepare = sqlite3_prepare_v2(handle_, insertSql, -1, &rawInsert, nullptr);
        std::unique_ptr<sqlite3_stmt, decltype(finalize)> insertStatement(rawInsert, finalize);
        checkSqlite(insertPrepare, handle_, "Unable to prepare market candle insert");

        for (const auto& candle : candles) {
            sqlite3_reset(insertStatement.get());
            sqlite3_clear_bindings(insertStatement.get());
            checkSqlite(sqlite3_bind_text(insertStatement.get(), 1, keyString.c_str(), -1, SQLITE_TRANSIENT),
                        handle_, "Unable to bind candle cache key");
            checkSqlite(sqlite3_bind_text(insertStatement.get(), 2, candle.symbol.c_str(), -1, SQLITE_TRANSIENT),
                        handle_, "Unable to bind candle symbol");
            checkSqlite(sqlite3_bind_text(insertStatement.get(), 3, candle.source.c_str(), -1, SQLITE_TRANSIENT),
                        handle_, "Unable to bind candle source");
            checkSqlite(sqlite3_bind_text(insertStatement.get(), 4, candle.interval.c_str(), -1, SQLITE_TRANSIENT),
                        handle_, "Unable to bind candle interval");
            checkSqlite(sqlite3_bind_int64(insertStatement.get(), 5, toEpochMilliseconds(candle.startTime)),
                        handle_, "Unable to bind candle start timestamp");
            checkSqlite(sqlite3_bind_int64(insertStatement.get(), 6, toEpochMilliseconds(candle.endTime)),
                        handle_, "Unable to bind candle end timestamp");
            checkSqlite(sqlite3_bind_double(insertStatement.get(), 7, candle.open),
                        handle_, "Unable to bind candle open");
            checkSqlite(sqlite3_bind_double(insertStatement.get(), 8, candle.high),
                        handle_, "Unable to bind candle high");
            checkSqlite(sqlite3_bind_double(insertStatement.get(), 9, candle.low),
                        handle_, "Unable to bind candle low");
            checkSqlite(sqlite3_bind_double(insertStatement.get(), 10, candle.close),
                        handle_, "Unable to bind candle close");
            checkSqlite(sqlite3_bind_int64(insertStatement.get(), 11, toEpochMilliseconds(candle.fetchedAt)),
                        handle_, "Unable to bind candle fetched timestamp");
            const int insertStep = sqlite3_step(insertStatement.get());
            if (insertStep != SQLITE_DONE) {
                checkSqlite(insertStep, handle_, "Unable to save market candle");
            }
        }
        execute("COMMIT");
    } catch (...) {
        char* ignored = nullptr;
        sqlite3_exec(handle_, "ROLLBACK", nullptr, nullptr, &ignored);
        sqlite3_free(ignored);
        throw;
    }
}

std::vector<OhlcCandle> Database::loadMarketCandles(std::string_view cacheKey) const {
    std::lock_guard lock(mutex_);
    constexpr const char* sql =
        "SELECT symbol, source, interval, start_ms, end_ms, open, high, low, close, fetched_at_ms "
        "FROM market_candles WHERE cache_key = ?1 ORDER BY start_ms ASC";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(handle_, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> prepared(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare market candle query");

    const std::string keyString(cacheKey);
    checkSqlite(sqlite3_bind_text(prepared.get(), 1, keyString.c_str(), -1, SQLITE_TRANSIENT),
                handle_, "Unable to bind market candle cache key");
    std::vector<OhlcCandle> candles;
    for (;;) {
        const int stepResult = sqlite3_step(prepared.get());
        if (stepResult == SQLITE_DONE) {
            break;
        }
        if (stepResult != SQLITE_ROW) {
            checkSqlite(stepResult, handle_, "Unable to read market candles");
        }
        candles.push_back(OhlcCandle{
            columnText(prepared.get(), 0),
            columnText(prepared.get(), 1),
            columnText(prepared.get(), 2),
            fromEpochMilliseconds(sqlite3_column_int64(prepared.get(), 3)),
            fromEpochMilliseconds(sqlite3_column_int64(prepared.get(), 4)),
            sqlite3_column_double(prepared.get(), 5),
            sqlite3_column_double(prepared.get(), 6),
            sqlite3_column_double(prepared.get(), 7),
            sqlite3_column_double(prepared.get(), 8),
            fromEpochMilliseconds(sqlite3_column_int64(prepared.get(), 9)),
            true,
        });
    }
    return candles;
}

bool Database::enqueueAlert(std::string_view dedupeKey, std::string_view category,
                            std::string_view message, std::string_view sourceUrl,
                            const std::vector<std::int64_t>& userIds,
                            std::chrono::system_clock::time_point createdAt) {
    std::lock_guard lock(mutex_);
    if (dedupeKey.empty() || category.empty() || message.empty()) {
        throw AppError(ErrorCode::Configuration, "Scheduled alert fields must not be empty");
    }
    execute("BEGIN IMMEDIATE");
    try {
        bool inserted = false;
        const auto alertId = insertAlert(handle_, dedupeKey, category, message, sourceUrl,
                                         createdAt, inserted);
        if (inserted) {
            insertDeliveries(handle_, alertId, userIds, createdAt);
        }
        execute("COMMIT");
        return inserted;
    } catch (...) {
        char* ignored = nullptr;
        sqlite3_exec(handle_, "ROLLBACK", nullptr, nullptr, &ignored);
        sqlite3_free(ignored);
        throw;
    }
}

bool Database::recordSignalTransition(
    std::string_view market, std::string_view state, std::string_view dedupeKey,
    std::string_view message, const std::vector<std::int64_t>& userIds,
    std::chrono::system_clock::time_point createdAt) {
    std::lock_guard lock(mutex_);
    if (market.empty() || (state != "BUY SETUP" && state != "SELL SETUP" && state != "WAIT")) {
        throw AppError(ErrorCode::Configuration, "Invalid scheduled signal state");
    }
    execute("BEGIN IMMEDIATE");
    try {
        std::optional<std::string> previous;
        sqlite3_stmt* rawPrevious = nullptr;
        const int prepareResult = sqlite3_prepare_v2(
            handle_, "SELECT state FROM monitor_signal_state WHERE market = ?1", -1,
            &rawPrevious, nullptr);
        const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
        std::unique_ptr<sqlite3_stmt, decltype(finalize)> previousStatement(rawPrevious, finalize);
        checkSqlite(prepareResult, handle_, "Unable to prepare monitor state query");
        bindText(previousStatement.get(), 1, market, handle_, "Unable to bind monitor market");
        const int previousStep = sqlite3_step(previousStatement.get());
        if (previousStep == SQLITE_ROW) {
            previous = columnText(previousStatement.get(), 0);
        } else if (previousStep != SQLITE_DONE) {
            checkSqlite(previousStep, handle_, "Unable to read monitor state");
        }

        sqlite3_stmt* rawState = nullptr;
        const int statePrepare = sqlite3_prepare_v2(
            handle_, "INSERT INTO monitor_signal_state(market, state, updated_at_ms) VALUES(?1, ?2, ?3) "
                     "ON CONFLICT(market) DO UPDATE SET state = excluded.state, updated_at_ms = excluded.updated_at_ms",
            -1, &rawState, nullptr);
        std::unique_ptr<sqlite3_stmt, decltype(finalize)> stateStatement(rawState, finalize);
        checkSqlite(statePrepare, handle_, "Unable to prepare monitor state update");
        bindText(stateStatement.get(), 1, market, handle_, "Unable to bind monitor market");
        bindText(stateStatement.get(), 2, state, handle_, "Unable to bind monitor signal state");
        checkSqlite(sqlite3_bind_int64(stateStatement.get(), 3, toEpochMilliseconds(createdAt)),
                    handle_, "Unable to bind monitor state timestamp");
        const int stateStep = sqlite3_step(stateStatement.get());
        if (stateStep != SQLITE_DONE) {
            checkSqlite(stateStep, handle_, "Unable to update monitor state");
        }

        bool inserted = false;
        if (state != "WAIT" && (!previous || *previous != state)) {
            const auto alertId = insertAlert(handle_, dedupeKey, "paper_signal", message, "",
                                             createdAt, inserted);
            if (inserted) {
                insertDeliveries(handle_, alertId, userIds, createdAt);
            }
        }
        execute("COMMIT");
        return inserted;
    } catch (...) {
        char* ignored = nullptr;
        sqlite3_exec(handle_, "ROLLBACK", nullptr, nullptr, &ignored);
        sqlite3_free(ignored);
        throw;
    }
}

std::vector<AlertDelivery> Database::loadDueAlertDeliveries(
    std::chrono::system_clock::time_point asOf, std::size_t limit) const {
    std::lock_guard lock(mutex_);
    constexpr const char* sql =
        "SELECT d.alert_id, d.user_id, a.message, d.attempts "
        "FROM scheduler_alert_deliveries d JOIN scheduler_alerts a ON a.id = d.alert_id "
        "WHERE d.state = 'PENDING' AND d.next_attempt_ms <= ?1 "
        "ORDER BY a.created_at_ms, d.alert_id, d.user_id LIMIT ?2";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(handle_, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> statement(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare alert delivery query");
    checkSqlite(sqlite3_bind_int64(statement.get(), 1, toEpochMilliseconds(asOf)),
                handle_, "Unable to bind alert delivery time");
    const auto safeLimit = std::min<std::size_t>(limit, 1000);
    checkSqlite(sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(safeLimit)),
                handle_, "Unable to bind alert delivery limit");
    std::vector<AlertDelivery> deliveries;
    for (;;) {
        const int stepResult = sqlite3_step(statement.get());
        if (stepResult == SQLITE_DONE) {
            break;
        }
        if (stepResult != SQLITE_ROW) {
            checkSqlite(stepResult, handle_, "Unable to read alert deliveries");
        }
        deliveries.push_back({sqlite3_column_int64(statement.get(), 0),
                              sqlite3_column_int64(statement.get(), 1),
                              columnText(statement.get(), 2),
                              static_cast<std::size_t>(std::max<sqlite3_int64>(
                                  0, sqlite3_column_int64(statement.get(), 3)))});
    }
    return deliveries;
}

void Database::markAlertDelivered(std::int64_t alertId, std::int64_t userId,
                                  std::chrono::system_clock::time_point deliveredAt) {
    std::lock_guard lock(mutex_);
    constexpr const char* sql =
        "UPDATE scheduler_alert_deliveries SET state = 'SENT', delivered_at_ms = ?3, "
        "last_error_category = '' WHERE alert_id = ?1 AND user_id = ?2 AND state = 'PENDING'";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(handle_, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> statement(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare delivery completion");
    checkSqlite(sqlite3_bind_int64(statement.get(), 1, alertId), handle_, "Unable to bind alert ID");
    checkSqlite(sqlite3_bind_int64(statement.get(), 2, userId), handle_, "Unable to bind alert recipient");
    checkSqlite(sqlite3_bind_int64(statement.get(), 3, toEpochMilliseconds(deliveredAt)),
                handle_, "Unable to bind delivery timestamp");
    const int stepResult = sqlite3_step(statement.get());
    if (stepResult != SQLITE_DONE) {
        checkSqlite(stepResult, handle_, "Unable to mark alert delivered");
    }
}

void Database::deferAlertDelivery(std::int64_t alertId, std::int64_t userId,
                                  std::chrono::system_clock::time_point nextAttempt,
                                  std::string_view errorCategory, bool permanent) {
    std::lock_guard lock(mutex_);
    if (!safeHistoryLabel(errorCategory)) {
        throw AppError(ErrorCode::Configuration, "Invalid scheduler error category");
    }
    constexpr const char* sql =
        "UPDATE scheduler_alert_deliveries SET state = ?4, attempts = attempts + 1, "
        "next_attempt_ms = ?3, last_error_category = ?5 WHERE alert_id = ?1 AND user_id = ?2 AND state = 'PENDING'";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(handle_, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> statement(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare alert retry update");
    checkSqlite(sqlite3_bind_int64(statement.get(), 1, alertId), handle_, "Unable to bind alert ID");
    checkSqlite(sqlite3_bind_int64(statement.get(), 2, userId), handle_, "Unable to bind alert recipient");
    checkSqlite(sqlite3_bind_int64(statement.get(), 3, toEpochMilliseconds(nextAttempt)),
                handle_, "Unable to bind retry timestamp");
    bindText(statement.get(), 4, permanent ? "FAILED" : "PENDING", handle_,
             "Unable to bind delivery status");
    bindText(statement.get(), 5, errorCategory, handle_, "Unable to bind delivery error category");
    const int stepResult = sqlite3_step(statement.get());
    if (stepResult != SQLITE_DONE) {
        checkSqlite(stepResult, handle_, "Unable to defer alert delivery");
    }
}

void Database::recordSchedulerError(std::string_view component, std::string_view errorCategory,
                                    std::chrono::system_clock::time_point occurredAt) {
    std::lock_guard lock(mutex_);
    if (!safeHistoryLabel(component) || !safeHistoryLabel(errorCategory)) {
        throw AppError(ErrorCode::Configuration, "Invalid scheduler error history label");
    }
    constexpr const char* sql =
        "INSERT INTO scheduler_errors(component, error_category, occurred_at_ms) VALUES(?1, ?2, ?3)";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(handle_, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> statement(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare scheduler error insert");
    bindText(statement.get(), 1, component, handle_, "Unable to bind scheduler component");
    bindText(statement.get(), 2, errorCategory, handle_, "Unable to bind scheduler error category");
    checkSqlite(sqlite3_bind_int64(statement.get(), 3, toEpochMilliseconds(occurredAt)),
                handle_, "Unable to bind scheduler error timestamp");
    const int stepResult = sqlite3_step(statement.get());
    if (stepResult != SQLITE_DONE) {
        checkSqlite(stepResult, handle_, "Unable to save scheduler error");
    }
    execute("DELETE FROM scheduler_errors WHERE id NOT IN "
            "(SELECT id FROM scheduler_errors ORDER BY occurred_at_ms DESC LIMIT 500)");
}

std::optional<std::string> Database::lastSchedulerError() const {
    std::lock_guard lock(mutex_);
    constexpr const char* sql =
        "SELECT component || ':' || error_category FROM scheduler_errors "
        "ORDER BY occurred_at_ms DESC, id DESC LIMIT 1";
    sqlite3_stmt* rawStatement = nullptr;
    const int prepareResult = sqlite3_prepare_v2(handle_, sql, -1, &rawStatement, nullptr);
    const auto finalize = [](sqlite3_stmt* item) { sqlite3_finalize(item); };
    std::unique_ptr<sqlite3_stmt, decltype(finalize)> statement(rawStatement, finalize);
    checkSqlite(prepareResult, handle_, "Unable to prepare scheduler health query");
    const int stepResult = sqlite3_step(statement.get());
    if (stepResult == SQLITE_DONE) {
        return std::nullopt;
    }
    if (stepResult != SQLITE_ROW) {
        checkSqlite(stepResult, handle_, "Unable to read scheduler health");
    }
    return columnText(statement.get(), 0);
}

} // namespace trading_bot
