# Gemini AI Trading Telegram Bot — Local C++20

Phase 8 adds a persistent local monitoring scheduler, important-news and educational paper-signal alerts, retry and deduplication state, scheduler diagnostics, and a macOS LaunchAgent option. The app remains a C++20 local process. It uses CoinGecko Demo and Gold API for market data, permitted publisher RSS feeds for headlines, and Gemini only for optional explanations when `GEMINI_API_KEY` is configured. It does not connect to a broker or place orders.

## Requirements and install

- macOS on Apple Silicon
- Xcode Command Line Tools and Homebrew
- CMake 3.20 or newer
- libcurl, nlohmann/json, SQLite3, and libxml2

```sh
xcode-select --install
brew install cmake ninja curl nlohmann-json sqlite libxml2
```

The project has no hosting requirement because it uses outbound long polling. Local development dependencies are available at no monthly cost. Telegram bot creation is done through Telegram's BotFather.

## Configure

```sh
cp .env.example .env
```

Set these values in `.env`:

| Variable | Purpose | Default |
| --- | --- | --- |
| `TELEGRAM_BOT_TOKEN` | Token from BotFather | unset; Telegram polling is disabled |
| `TELEGRAM_ALLOWED_USER_IDS` | Comma-separated positive Telegram user IDs allowed to use the bot | empty; bot refuses to start when a token is set |
| `TELEGRAM_LONG_POLL_TIMEOUT_SECONDS` | Telegram `getUpdates` long-poll timeout | `25` |
| `BOT_HTTP_TIMEOUT_SECONDS` | Base libcurl timeout; the bot adds a 5 second margin over the long-poll timeout when needed | `20` |
| `BOT_MARKET_CHECK_INTERVAL_SECONDS` | Market and paper-signal monitoring cadence; minimum 60 seconds | `300` |
| `BOT_NEWS_CHECK_INTERVAL_SECONDS` | RSS news check cadence; minimum 300 seconds | `1800` (30 minutes) |
| `BOT_TIMEZONE` | Installed IANA timezone used for monitoring timestamps | `UTC` |
| `BOT_DATABASE_PATH` | SQLite database file, including saved Telegram update offset | `data/trading_bot.sqlite3` |
| `BOT_LOG_LEVEL` | Minimum JSON log level | `info` |
| `BACKTEST_STARTING_BALANCE` | Starting virtual balance for historical replay | `10000` |
| `BACKTEST_FEE_BPS` | Simulated fee on each entry and exit fill, in basis points | `10` (0.10%) |
| `BACKTEST_SLIPPAGE_BPS` | Adverse simulated slippage on each fill, in basis points | `5` (0.05%) |
| `BACKTEST_RISK_FRACTION` | Virtual account risk budget per entry | `0.01` (1%) |
| `GEMINI_API_KEY` | Google AI Studio key for optional AI explanations | unset; RSS and technical analysis work without AI |
| `COINGECKO_DEMO_API_KEY` | Free CoinGecko Demo key for BTC price and OHLC | unset; BTC data is unavailable |
| `GOLD_API_KEY` | Free Gold API key for XAU/USD historical OHLC | unset; gold spot still works |

The process environment overrides values loaded from `.env`. After copying the file, lock down its permissions with `chmod 600 .env`. If a dotenv file contains a token, API key, or Telegram allowlist, the app requires the file to be owned by the current user and have no group/other permissions. SQLite database files are also restricted to owner read/write permissions. Get your numeric Telegram user ID from a trusted Telegram ID lookup bot or your own update payload, then add only intended users to the allowlist. An empty allowlist denies everyone. The bot replies only to authorized users in private chats; unauthorized updates and group messages receive no reply. Message text, credentials, and user IDs are not written to logs.

The code uses the standard Gemini `generateContent` endpoint with the stable `gemini-3.8-flash` model. Gemini, CoinGecko Demo, and Gold API offer free-tier access; a free-tier project has limits, and the bot reports quota exhaustion rather than retrying continuously. To keep Gemini at $0, use an API key attached to a free-tier project without billing. Google documents that free-tier content may be used to improve its products, so this bot sends only public RSS metadata and public indicator values to Gemini—never Telegram user IDs or private messages. It uses no search grounding or paid Gemini tools. Review Google's [Gemini API pricing and data-use terms](https://ai.google.dev/gemini-api/docs/pricing) before configuring a key. Gold API's spot endpoint needs no key and updates every 30 seconds. Its `/ohlc` endpoint requires a free key and is limited to 10 history/OHLC requests per hour; the bot caches the requested 24-hour range for one hour. CoinGecko Demo needs a free Demo key; spot is cached for 60 seconds and OHLC for five minutes. RSS bodies are cached for five minutes and AI summaries are cached locally in SQLite for 30 minutes by model and evidence content.

`.env`, database files, and common secret files are ignored by Git. Do not commit the bot token.

## Build, run, and test

```sh
./scripts/build.sh
./scripts/test.sh
./scripts/integration-test.sh
./scripts/run.sh
```

The scripts configure CMake, build, run the complete CTest suite, or run the mocked Phase 8 scheduler integration tests. They use Homebrew prefixes when Homebrew is installed. No Python runtime, hosting plan, paid market-data service, or paid Gemini API is required. If `TELEGRAM_BOT_TOKEN` is unset, the executable logs that polling is not configured and exits. Once configured, it long-polls Telegram, sends command replies only to allowlisted users, and saves the next update offset in SQLite after each processed update. Poll and scheduler network failures use bounded retry delays; Telegram 429 responses use `retry_after`. `SIGINT` and `SIGTERM` stop the scheduler and poll loop cleanly (the active Telegram long poll can take up to its configured timeout to return).

## Commands

| Command | Behavior |
| --- | --- |
| `/start` | Welcome message |
| `/help` | List available commands |
| `/status` | Scheduler, recent-check, pending/failed alert, local storage, and paper-only diagnostics |
| `/calendar` | Upcoming BLS release dates, official links, timezone, and delayed/unavailable status |
| `/gold` | Latest reported XAU/USD price and Gold API's requested 24-hour OHLC range |
| `/btc` | Latest reported BTC/USD price and the latest returned CoinGecko OHLC candle |
| `/analysis` | BTC and XAU technical indicators, current RSS context, and optional Gemini explanation |
| `/news` | Recent publisher headlines with source links and optional Gemini explanation |
| `/signal` | BTC and XAU/USD educational scenarios with BUY SETUP, SELL SETUP, or WAIT; validated directional scenarios may include Gemini explanation |
| `/history` | Latest completed historical backtest trades saved in SQLite |
| `/performance` | Replay the available BTC and XAU/USD history and report simulated performance |

`/gold` and `/btc` replies show the provider timestamp and data source; delayed cache fallbacks are marked `DATA DELAYED`. Missing spot prices or quote history are shown as `DATA UNAVAILABLE`. The bot does not describe provider data as real-time. Gold API returns one OHLC aggregate for the supplied start/end range, with no documented fixed candle cadence; replies label it as a requested 24-hour range. CoinGecko Demo's `/ohlc?days=1` returns provider OHLC rows with automatic 30-minute granularity. Demo supports lookbacks of 1, 7, 14, 30, 90, 180, and 365 days (up to one year); auto-granularity is 4 hours for 3–30 days and 4 days for 31 days or more. This bot requests one day only. See the providers' [CoinGecko Demo OHLC documentation](https://docs.coingecko.com/demo/reference/coins-id-ohlc) and [Gold API endpoint and free-tier details](https://gold-api.com/llms.txt).

`/analysis` currently evaluates only fixed provider candle intervals: 30 minutes (`30m`), 4 hours (`4h`), and 4 days (`4d`). CoinGecko's current one-day request provides 30-minute candles; the other intervals are accepted by the reusable analysis module when a provider supplies them. Gold's one requested 24-hour range is an aggregate, not a regular candle timeframe, so it returns `INSUFFICIENT DATA` for indicator analysis. At least 34 contiguous, complete candles are required for the default MACD (12, 26, 9) warm-up. A shorter, gapped, unsupported, or unfinished series reports `INSUFFICIENT DATA`; invalid OHLC is reported as `DATA UNAVAILABLE`.

The technical engine calculates SMA(20), SMA-seeded EMA(12/26), Wilder RSI(14), MACD(12,26,9), Wilder ATR(14), and support/resistance from the prior 20 candles. The latest candle is excluded from support/resistance; the engine rejects future or unfinished candles and never fills gaps or constructs candles from spot quotes. Trend classification is `BULLISH`, `BEARISH`, or `NEUTRAL` based on aligned close/EMA, MACD, and RSI conditions. Results are descriptive market analysis and do not provide trading instructions. Indicator warm-up and smoothing follow the SMA-seeded EMA and Wilder RSI/ATR conventions documented by [TA-Lib for EMA](https://github.com/TA-Lib/ta-lib/blob/main/docs/studies/ema-seeding/README.md), [RSI](https://github.com/TA-Lib/ta-lib/blob/main/ta_codegen/input/rsi/rsi.md), and [ATR](https://github.com/TA-Lib/ta-lib/blob/main/ta_codegen/input/atr/atr.md).

`/signal` is an educational paper-trading scenario, never a broker order or real-money instruction. It evaluates BTC and XAU/USD independently and reports `BUY SETUP`, `SELL SETUP`, or `WAIT`. A directional state requires complete, contiguous indicators and agreement from all four rules: technical trend, MACD histogram sign, RSI at or above 55 (bullish) or at or below 45 (bearish), and latest close above or below SMA(20). Any opposing directional rules are listed as conflicts; neutral or insufficient evidence produces `WAIT`.

Signals require a provider quote no older than 10 minutes, a quote and candle fetch no older than 15 minutes, no provider delay flag, and a latest completed candle no older than two intervals. The quote must also be within the greater of three ATRs or 2% of the latest candle close. Unsupported, missing, invalid, stale, or inconsistent data suppresses all scenario levels. Under the current provider setup, BTC's supported 30-minute OHLC can qualify; Gold's 24-hour aggregate is not a fixed candle interval and therefore remains `WAIT` for signals until fixed-interval gold history is available.

For an aligned setup, the hypothetical entry zone is centered on the current provider quote and spans 0.1 ATR on either side. The illustrative stop is 1 ATR beyond the far edge of that zone; TP1 and TP2 are placed at 1R and 2R from the zone midpoint, yielding displayed ratios of 1:1 and 1:2. These are deterministic illustrations, not confidence estimates, guarantees, or recommendations. A `WAIT` result never includes entry, stop, or target levels. Every `/signal` evaluation, including `WAIT`, is saved to the local `signal_scenarios` SQLite table with computed indicator values, rules, conflicts, data source, and timestamps. Gemini may explain only a validated directional setup using its indicators and existing levels; its response cannot change the state or levels, and the app skips AI commentary for `WAIT` results.

`/performance` runs a historical replay only. It needs at least 35 complete, contiguous candles: 34 candles for indicator warm-up and a later bar for possible execution. Each decision uses only the candle prefix available at that close; entry is attempted at the next candle's open and only if that open is inside the earlier hypothetical entry zone. Slippage must leave the simulated fill inside the zone. No later candle is used to decide an earlier signal or entry. OHLC bars do not show intrabar ordering, so a bar that touches both a stop and a target resolves stop-first. The simulator exits half the virtual position at TP1 and the remainder at TP2, a stop, or the end of available history. Slippage is adverse, and fees apply on every fill.

The default replay account starts at a virtual `$10,000`, risks at most 1% of current virtual balance per entry, charges 10 bps per fill, and applies 5 bps of slippage per fill. Override these values with the `BACKTEST_*` environment variables. Position notional is capped by virtual account balance; the simulator uses no leverage or external account. Performance reports show closed trade count, positive-net win rate, average net realized R, fees, virtual P&L, and maximum drawdown. Metrics are unavailable if history is insufficient or no trades close. Reports show the available sample size and date range; results are simulations, not predictions.

`/performance` and `/history` use separate `backtest_runs` and `backtest_trades` SQLite tables. Replaying identical candles with identical settings is idempotent. Phase 6 `/signal` scenarios stay in their own `signal_scenarios` table and are never counted as backtest positions. No live orders or broker connections exist. Forward paper-position tracking is not implemented; `/signal` remains an informational scenario, not an open virtual position. Current BTC history is limited to the provider's one-day, 30-minute feed, so the replay sample is small. Gold's 24-hour aggregate is not a fixed candle interval and remains insufficient for backtesting.

`/news` reads only dated headline/source/link metadata from the [CoinDesk RSS feed](https://www.coindesk.com/coindesk-news/2021/09/17/coindesk-rss), four [Federal Reserve RSS feeds](https://www.federalreserve.gov/feeds/feeds.htm), and the [BLS Latest Numbers RSS feed](https://www.bls.gov/feed/bls_latest.rss). Items older than 72 hours, items with future or missing dates, unsafe URLs, and duplicate headlines/links are excluded. Article pages and full article text are not fetched. Telegram output includes source URLs and publication timestamps and labels these as publisher reports, not independently verified facts.

`/calendar` is implemented against the public [BLS economic-release iCalendar](https://www.bls.gov/schedule/news_release/bls.ics), including event times in America/New_York and official release links when supplied. Calendar responses are cached in SQLite for six hours; if refresh fails, validated cache entries are marked delayed. In the live check for this version, BLS returned an Access Denied page that says automated retrieval must conform to BLS policy; with no prior cache, the command reports `DATA UNAVAILABLE`. The bot does not attempt to work around that restriction. BLS dates can change, so the official schedule remains authoritative.

Use `./build/trading_bot --verify-integrations` for a read-only connectivity/data check after building. It reports credential states without displaying credential values, requests the Telegram `getMe` identity endpoint only (no messages), checks Gemini model metadata only (no content-generation request), and performs public RSS/BLS/Gold checks. When credentials are configured it also checks CoinGecko Demo and may make one Gold OHLC request to validate the optional history key. The verifier uses an in-memory database and does not start polling or the scheduler. Missing credentials are reported as `NOT_CONFIGURED`; quota, stale data, unavailable feeds, and malformed responses are reported as status values. No billing setting is modified. The verifier cannot establish whether a provider account has billing enabled; keep Gemini on a free-tier project if maintaining a $0 target.

Gemini receives only the selected headlines, publisher names, publication timestamps, and structured technical-analysis values. Headlines are untrusted data in the user-content field; fixed safety instructions are sent separately using Gemini's system-instruction field. The prompt asks for separate `Facts`, `Possible implications (speculation)`, and `Uncertainty` sections. Responses missing those sections or containing direct trading directives are rejected. Gemini output cannot provide source URLs; Telegram displays the RSS links directly. `gemini-3.8-flash` and the `generateContent` API are documented by [Google's model guide](https://ai.google.dev/gemini-api/docs/models/gemini-3.8-flash) and [API reference](https://ai.google.dev/api/generate-content). Gemini output is not a guaranteed prediction or real-money trading instruction.

Deterministic tests cover RSS parsing, Gemini request/authentication, market data, technical indicator references, signal alignment and conflicts, stale/malformed data rejection, and SQLite persistence. Backtest tests cover causal prefix replay, next-bar execution, long/short exits, fees, slippage, stop-first ambiguous bars, P&L accounting, win rate, drawdown, idempotent history, and Telegram `/history` and `/performance` summaries.

Unknown commands receive a short `/help` hint. No command performs live trading. Provider OHLC values are stored in SQLite; spot prices are never converted into synthetic candles.

## Phase 8 monitoring and notifications

The scheduler starts alongside Telegram long polling. It checks BTC and XAU/USD at `BOT_MARKET_CHECK_INTERVAL_SECONDS` (default 5 minutes) and reads the configured RSS feeds every `BOT_NEWS_CHECK_INTERVAL_SECONDS` (default 30 minutes). Current provider caches and free-tier restrictions still apply; the scheduler does not bypass provider quotas or turn a cached/stale quote into a fresh candle. Failed checks and delivery errors are stored in `scheduler_errors` with sanitized component/category labels. Alert records, per-recipient delivery state, retry times, and deduplication keys are stored in `scheduler_alerts` and `scheduler_alert_deliveries`.

Paper-signal alerts are queued only for a `BUY SETUP` or `SELL SETUP` whose signal engine data status is `READY`; `WAIT`, stale, delayed, incomplete, or invalid inputs never send a signal alert. The scheduler sends again only after the market leaves a setup state and a later valid setup appears. This signal state is persisted across restarts. News alerts use a conservative headline keyword filter, include the publisher, publication time, and source URL, and explicitly label the item as a publisher report with uncertain implications. Duplicate RSS links are suppressed in SQLite. Each private allowlisted chat is paced to at most one message per second. HTTP 429 responses are delayed by Telegram's `retry_after`, and transient failures use bounded backoff. Because Telegram `sendMessage` has no idempotency token, an ambiguous connection loss after Telegram accepted a message can still make a retried delivery appear twice; the scheduler prevents duplicate alert creation but cannot provide exactly-once network delivery.

`/status` reports scheduler state, timezone, configured check intervals, last check times, pending and permanently failed alert deliveries, last sanitized scheduler error, and SQLite availability. It does not make a live probe of every external provider; provider connectivity is reflected when the next scheduled check runs. Telegram's existing long poll and market/news provider caches keep outbound requests low. No AI request is made for scheduled news filtering or signal generation, so Gemini is not needed for alerts.

The current free provider configuration has one practical data limitation: CoinGecko Demo returns BTC OHLC at 30-minute cadence for the requested one-day history; Gold API supplies a 24-hour aggregate, not fixed-timeframe candles. Therefore BTC can produce eligible signal alerts when current data aligns, while XAU/USD remains `WAIT` for fixed-timeframe signal/indicator work until a supported free historical feed is configured. The scheduler reports no alert when evidence is unavailable.

## Local macOS operation

The Mac must be awake, logged in, and connected to the internet for continuous alerts. For a foreground session, run `./scripts/run.sh` from the project directory and leave that terminal running. Use `Control-C` to stop cleanly. For user-session startup and automatic process restart after an exit, build and configure `.env` first, then generate and load a LaunchAgent:

```sh
chmod 600 .env
./scripts/build.sh
./scripts/install_launch_agent.sh
launchctl bootstrap "gui/$(id -u)" "$HOME/Library/LaunchAgents/com.local.gemini-telegram-trading-bot.plist"
launchctl print "gui/$(id -u)/com.local.gemini-telegram-trading-bot"
```

Restart the loaded agent with `launchctl kickstart -k "gui/$(id -u)/com.local.gemini-telegram-trading-bot"`. Stop and unload it with `launchctl bootout "gui/$(id -u)/com.local.gemini-telegram-trading-bot"`. The LaunchAgent contains only paths and restart settings; credentials remain in the ignored, owner-only `.env` file. Standard output and errors go to `~/Library/Logs/GeminiTelegramTradingBot/`. LaunchAgents run in the logged-in user's session; sleep, logout, power loss, or a network outage pauses monitoring. Launchd restarts the process after exit, while the app itself retries transient Telegram connection failures. Apple documents LaunchAgent registration and lifecycle in its [Service Management documentation](https://developer.apple.com/documentation/servicemanagement/smappservice). No cloud host is involved, so the development/hosting target remains $0/month; provider free-tier availability and Telegram connectivity are external dependencies.

The configured timezone must be installed on macOS (for example `Asia/Phnom_Penh` or `America/Los_Angeles`). The main process applies it before starting the scheduler. Market/news checks run immediately after startup, then follow the configured intervals; the scheduler persists completed check times so an ordinary process restart does not immediately re-fetch data that was just checked.

## Tests and deployment limits

`./scripts/test.sh` builds the project and runs all deterministic tests. `./scripts/integration-test.sh` runs the end-to-end Phase 8 scheduler test with mocked RSS, market data, and Telegram responses; it performs no live API requests. Test coverage includes allowlist-only recipients, signal state transitions, news links, persistent deduplication after scheduler reconstruction, Telegram per-chat pacing, 429 `retry_after`, error history, configuration bounds, and clean stop behavior.

Continuous alerts depend on the Mac staying powered, awake, logged in, and online. Free provider quotas, RSS publisher changes, network delivery ambiguity, and the current gold candle limitation can reduce or delay notifications. Scheduled alerts do not contain Gemini-generated market commentary, and there is no forward virtual-position lifecycle, broker connection, or real-money execution.

## Project layout

```text
include/trading_bot/   Public C++ interfaces, including technical, signal, and backtest engines
src/                   C++20 implementations, market providers, RSS, Gemini, simulations, and executable
tests/                 Unit tests with mocked Telegram, RSS, Gemini, and market responses
```
