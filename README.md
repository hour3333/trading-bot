# Gemini AI Trading Telegram Bot — Local C++20

Railway deployment: see [RAILWAY_DEPLOY.md](RAILWAY_DEPLOY.md).

Phase 8 adds a persistent local monitoring scheduler, important-news and educational paper-signal alerts, retry and deduplication state, scheduler diagnostics, and a macOS LaunchAgent option. Signal output now uses a validated, timeframe-aware format with explicit confidence and risk assessments. The app remains a C++20 local process. It uses CoinGecko Demo and Gold API for market data, permitted publisher RSS feeds for headlines, and Gemini only for optional explanations when `GEMINI_API_KEY` is configured. It does not connect to a broker or place orders.

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
| `BOT_NEWS_CHECK_INTERVAL_SECONDS` | RSS news check cadence; minimum 300 seconds | `300` (5 minutes) |
| `BOT_NEWS_ALERTS_PER_HOUR` | Maximum automatic news notifications per authorized user | `6` |
| `BOT_NEWS_GEMINI_SUMMARIES_PER_HOUR` | Maximum automatic gold-news Gemini summary requests | `6` |
| `BOT_TIMEZONE` | Installed IANA timezone used for signal and monitoring timestamps | `Asia/Phnom_Penh` |
| `BOT_SIGNAL_MARKET` | Paper-signal market; this deployment accepts gold only | `XAU/USD` |
| `BOT_SIGNAL_TIMEFRAME` | Configured signal interval (`M5`, `M15`, `H1`, or `H4`) | `M5` |
| `BOT_ALERT_MODE` | `CONFIRMED_ONLY` or `OFF` | `CONFIRMED_ONLY` |
| `BOT_TRADING_MODE` | Strictly paper-only; any other value is rejected | `PAPER_ONLY` |
| `BOT_AI_PROVIDER` | Explanation provider | `GEMINI` |
| `BOT_AUTO_SIGNAL_ANALYSIS` | Run the configured signal analysis on scheduler checks | `true` |
| `BOT_ALLOW_DUPLICATE_SIGNAL_ALERTS` | Permit same-state alerts on new candles | `false` |
| `BOT_SIGNAL_ALERT_COOLDOWN_SECONDS` | Minimum gap between gold signal alerts | `300` |
| `BOT_SIGNAL_MIN_RISK_REWARD` | Minimum TP1 risk/reward ratio; range `0.5`–`10` | `1.5` |
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

The code uses the standard Gemini `generateContent` endpoint with the stable `gemini-3.8-flash` model. Gemini, CoinGecko Demo, and Gold API offer free-tier access; a free-tier project has limits, and the bot reports quota exhaustion rather than retrying continuously. To keep Gemini at $0, use an API key attached to a free-tier project without billing. Google documents that free-tier content may be used to improve its products, so this bot sends only public RSS metadata and public indicator values to Gemini—never Telegram user IDs or private messages. It uses no search grounding or paid Gemini tools. Review Google's [Gemini API pricing and data-use terms](https://ai.google.dev/gemini-api/docs/pricing) before configuring a key. Gold API's spot endpoint needs no key and updates every 30 seconds. Its `/ohlc` endpoint requires a free key and is limited to 10 history/OHLC requests per hour; the bot caches the requested 24-hour range for one hour. CoinGecko Demo needs a free Demo key; spot is cached for 60 seconds, one-day 30-minute OHLC for five minutes, and 30-day 4-hour OHLC for ten minutes. RSS bodies are cached for five minutes and AI summaries are cached locally in SQLite for 30 minutes by model and evidence content.

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
| `/analysis` | Latest validated XAU/USD M5 analysis, sourced gold news, and optional Gemini explanation |
| `/analysis_on`, `/analysis_off` | Enable or disable your automatic significant gold M5 analysis alerts |
| `/analysis_status` | Show analysis alert preference, last update, status, and M5 data freshness |
| `/analysis_h4` | Legacy BTC and XAU H4 indicators with current RSS context and optional Gemini explanation |
| `/news` | Latest gold-relevant publisher headlines with impact class, source links, and optional Gemini context |
| `/news_on`, `/news_off` | Enable or disable your automatic gold-news alerts |
| `/news_status` | Show your alert mode, poll interval, and notification limit |
| `/news_high`, `/news_all` | Select HIGH-only or HIGH-and-MEDIUM alerts |
| `/news_test` | Send a clearly labeled diagnostic response to the authorized chat |
| `/signal` | Latest configured XAU/USD educational paper setup; defaults to M5 |
| `/m5` | XAU/USD M5 technical analysis; reports missing M5 candles explicitly |
| `/signal_m5` | M5 scenarios only when the configured source supplies genuine, complete M5 candles |
| `/signal_m15` | M15 scenarios only when the configured source supplies genuine, complete M15 candles |
| `/signal_h1` | H1 scenarios; provider candles may be aggregated only from complete lower-interval OHLC bars |
| `/signal_h4` | H4 scenarios |
| `/history` | Saved paper-signal scenarios and simulated backtest trades from SQLite |
| `/performance` | Replay the available BTC and XAU/USD history and report simulated performance |

`/gold` and `/btc` replies show provider timestamps and data sources; cached fallback data is marked delayed. Gold API returns a requested-range OHLC aggregate, not fixed M5 candles. Its FAQ documents minute/hour groupings as a paid feature, so this implementation will not request or construct substitute gold M5 bars. On the configured free provider, genuine XAU/USD M5 history is **not available**: `/signal`, `/m5`, and automatic alerts therefore report `DATA UNAVAILABLE` or `WAIT`, with no entry/stop/target levels. See [Gold API's free-tier and interval FAQ](https://gold-api.com/faqs). CoinGecko Demo's OHLC endpoint documents auto-granularity as 30 minutes for 1–2 days and 4 hours for 3–30 days; other BTC commands remain available as legacy market-data features.

`/analysis` and `/m5` calculate XAU/USD M5 indicators only from genuine fixed-interval, completed five-minute candles. At least 50 contiguous, complete bars are required because EMA(50) is part of the trend analysis. A shorter or gapped series reports `INSUFFICIENT DATA`; missing, unsupported, invalid, delayed, or stale M5 data reports `DATA UNAVAILABLE` or `STALE DATA`, and the message suppresses all directional levels. The configured free Gold API endpoint currently returns a requested-range aggregate rather than fixed M5 candles, so this bot's current provider setup cannot produce a validated gold M5 analysis. It will keep reporting the limitation until a configured provider returns actual M5 OHLC. `/analysis_h4` preserves the earlier BTC/XAU H4 analysis. The reusable analysis module also recognizes M15, H1, 30m, H4, and 4d intervals when returned by a provider.

The scheduler checks the gold M5 analysis every five minutes and refreshes it when a newly detected important gold story arrives. Analysis notification preferences are stored per authorized Telegram user and default to OFF; use `/analysis_on` to opt in. Alerts are de-duplicated and sent only for a significant validated change or an important news-triggered reassessment. `/analysis_off` disables those alerts, and `/analysis_status` shows the last recorded update and candle freshness. News notifications and paper-signal alerts have separate preferences and queues. If no genuine fresh M5 candles are available, the scheduler stores the unavailable status and sends no directional analysis alert.

The technical engine calculates SMA(20), trend EMA(20/50), MACD's EMA(12/26/9), Wilder RSI(14), MACD(12,26,9), Wilder ATR(14), and support/resistance from the prior 20 candles. The latest candle is excluded from support/resistance; the engine rejects future or unfinished candles and never fills gaps or constructs candles from spot quotes. Trend classification is `BULLISH`, `BEARISH`, or `NEUTRAL` based on close versus EMA(20/50), MACD, and RSI. Results are descriptive market analysis and do not provide trading instructions. Indicator warm-up and smoothing follow the SMA-seeded EMA and Wilder RSI/ATR conventions documented by [TA-Lib for EMA](https://github.com/TA-Lib/ta-lib/blob/main/docs/studies/ema-seeding/README.md), [RSI](https://github.com/TA-Lib/ta-lib/blob/main/ta_codegen/input/rsi/rsi.md), and [ATR](https://github.com/TA-Lib/ta-lib/blob/main/ta_codegen/input/atr/atr.md).

`/signal` is the XAU/USD paper-signal path, configured for M5 by default. The configurable `BOT_SIGNAL_TIMEFRAME` accepts M5, M15, H1, or H4, but it never changes timeframe silently. Legacy `/signal_m5`, `/signal_m15`, `/signal_h1`, and `/signal_h4` remain for manual BTC/XAU scenario inspection. `BOT_TRADING_MODE` accepts only `PAPER_ONLY`; no live order implementation exists. The deterministic signal requires a complete, fresh provider quote and matching completed candles, EMA(20/50) trend agreement, MACD(12/26/9) histogram direction, RSI(14) at least 55 for bullish or at most 45 for bearish, close above/below SMA(20), and a close breakout above prior 20-candle resistance for BUY or below prior 20-candle support for SELL. The latest candle is excluded from those prior-range levels. All five checks must align; conflicting, neutral, or missing evidence produces `WAIT` without price levels.

Signals require a quote no older than 10 minutes, quote and candle fetches no older than 15 minutes, no provider delay flag, a completed candle no older than two intervals, and quote/candle-close agreement within the larger of three ATRs or 2%. Time order, uniqueness, interval alignment, OHLC consistency, and a 50-candle EMA(50) warm-up are required. Unsupported, missing, stale, delayed, gapped, incomplete, or inconsistent data suppresses all levels and is reported with an explicit data status. Gold API's free OHLC is one requested-range aggregate, not M5 candles; the bot does not call it M5 or derive bars from spot snapshots. M5 will stay `DATA UNAVAILABLE` until a configured source actually returns genuine completed XAU/USD M5 OHLC.

For a confirmed setup, the hypothetical entry zone is centered on the current provider quote and spans 0.1 ATR on either side. The stop is 1 ATR beyond the far entry-zone edge. TP1 uses the configurable `BOT_SIGNAL_MIN_RISK_REWARD` (default 1:1.5) from the entry midpoint; TP2 uses the larger of 1:2 or 0.5R beyond the configured TP1 ratio. Prices and ratios are validated before output. BUY/SELL ordering, indicator agreement, data freshness, candle completeness, breakout structure, and minimum risk/reward must all pass. Confidence labels are qualitative: `High` means all five deterministic checks align; `Medium` means three or four checks align without directional conflict, `Low` means fewer than three align or indicators conflict, and `Unconfirmed` applies when data is not eligible. Risk is `Low` below 1% ATR/stop distance relative to close, `Medium` from 1% to below 2%, and `High` at or above 2%; the message includes the rule explanation. A `WAIT` result never has entry, stop, or target levels. Manual and automatic validated scenarios are stored in SQLite; `/history` distinguishes them from backtest trades. Gemini only explains the already-computed setup and relevant dated RSS metadata; it cannot change the signal, prices, checks, or ratios. AI text is filtered before display, and publisher links stay in the Telegram message outside Gemini output. These are illustrations, not performance claims or trading instructions.

`/performance` runs a historical replay only. It needs at least 51 complete, contiguous candles: 50 candles for EMA(50) warm-up and a later bar for possible execution. Each decision uses only the candle prefix available at that close; entry is attempted at the next candle's open and only if that open is inside the earlier hypothetical entry zone. Slippage must leave the simulated fill inside the zone. No later candle is used to decide an earlier signal or entry. OHLC bars do not show intrabar ordering, so a bar that touches both a stop and a target resolves stop-first. The simulator exits half the virtual position at TP1 and the remainder at TP2, a stop, or the end of available history. Slippage is adverse, and fees apply on every fill.

The default replay account starts at a virtual `$10,000`, risks at most 1% of current virtual balance per entry, charges 10 bps per fill, and applies 5 bps of slippage per fill. Override these values with the `BACKTEST_*` environment variables. Position notional is capped by virtual account balance; the simulator uses no leverage or external account. Performance reports show closed trade count, positive-net win rate, average net realized R, fees, virtual P&L, and maximum drawdown. Metrics are unavailable if history is insufficient or no trades close. Reports show the available sample size and date range; results are simulations, not predictions.

`/performance` and `/history` keep backtest runs/trades separate from saved scenarios. The current provider cannot supply XAU/USD M5 data, so there is no valid M5 gold backtest sample to report. Existing BTC H4 historical replay remains available as a separate legacy analysis and must not be treated as gold M5 validation. No brokerage or live execution exists; forward paper-position lifecycle is not implemented. A backtest is only a deterministic replay against the supplied historical candle sample and does not establish profitability.

`/news` reads only dated headline/source/link metadata from the [CoinDesk RSS feed](https://www.coindesk.com/coindesk-news/2021/09/17/coindesk-rss), four [Federal Reserve RSS feeds](https://www.federalreserve.gov/feeds/feeds.htm), and the [BLS Latest Numbers RSS feed](https://www.bls.gov/feed/bls_latest.rss). Items older than 72 hours, items with future or missing dates, unsafe URLs, and duplicate headlines/links are excluded. Article pages and full article text are not fetched. Telegram output includes source URLs and publication timestamps and labels these as publisher reports, not independently verified facts.

`/calendar` is implemented against the public [BLS economic-release iCalendar](https://www.bls.gov/schedule/news_release/bls.ics), including event times in America/New_York and official release links when supplied. Calendar responses are cached in SQLite for six hours; if refresh fails, validated cache entries are marked delayed. In the live check for this version, BLS returned an Access Denied page that says automated retrieval must conform to BLS policy; with no prior cache, the command reports `DATA UNAVAILABLE`. The bot does not attempt to work around that restriction. BLS dates can change, so the official schedule remains authoritative.

Use `./build/trading_bot --verify-integrations` for a read-only connectivity/data check after building. It reports credential states without displaying credential values, requests the Telegram `getMe` identity endpoint only (no messages), checks Gemini model metadata only (no content-generation request), and performs public RSS/BLS/Gold checks. When credentials are configured it also checks CoinGecko Demo and may make one Gold OHLC request to validate the optional history key. The verifier uses an in-memory database and does not start polling or the scheduler. Missing credentials are reported as `NOT_CONFIGURED`; quota, stale data, unavailable feeds, and malformed responses are reported as status values. No billing setting is modified. The verifier cannot establish whether a provider account has billing enabled; keep Gemini on a free-tier project if maintaining a $0 target.

Gemini receives only the selected headlines, publisher names, publication timestamps, and structured technical-analysis values. Headlines are untrusted data in the user-content field; fixed safety instructions are sent separately using Gemini's system-instruction field. The prompt asks for separate `Facts`, `Possible implications (speculation)`, and `Uncertainty` sections. Responses missing those sections or containing direct trading directives are rejected. Gemini output cannot provide source URLs; Telegram displays the RSS links directly. `gemini-3.8-flash` and the `generateContent` API are documented by [Google's model guide](https://ai.google.dev/gemini-api/docs/models/gemini-3.8-flash) and [API reference](https://ai.google.dev/api/generate-content). Gemini output is not a guaranteed prediction or real-money trading instruction.

Deterministic tests cover RSS parsing, Gemini request/authentication and response rejection, market data, indicator reference calculations, genuine interval handling, missing M5 rejection, incomplete/stale OHLC, signal alignment/conflicts, minimum RR enforcement, invalid price levels, Telegram formatting, duplicate/cooldown alert handling, schema migration, status diagnostics, and SQLite signal history. Backtest tests cover causal prefix replay, next-bar execution, fees, slippage, accounting, drawdown, and separation from saved scenarios. Tests use fixtures and mocked endpoints; they do not establish trading effectiveness or profitability.

Unknown commands receive a short `/help` hint. No command performs live trading. Provider OHLC values are stored in SQLite; spot prices are never converted into synthetic candles.

## Phase 8 monitoring and notifications

The scheduler starts alongside Telegram long polling. It checks the configured gold timeframe at `BOT_MARKET_CHECK_INTERVAL_SECONDS` (default 5 minutes, matching M5 cadence) and reads RSS feeds every `BOT_NEWS_CHECK_INTERVAL_SECONDS` (default 5 minutes). The poll interval must be at least five minutes to respect the publisher feeds and local RSS cache. A full feed failure retries on a persisted 60, 120, 240, then 300-second backoff; success resumes the configured interval. Provider caches and free-tier quotas apply; the scheduler never upgrades stale quotes into candles. Failed checks and delivery errors are stored in SQLite using sanitized categories.

Signal alerts default to `CONFIRMED_ONLY`, `PAPER_ONLY`, and duplicate suppression. Only a validated BUY/SELL for the configured market/timeframe can enter the queue. WAIT, missing, stale, delayed, incomplete, invalid, or conflicting input never alerts. The scheduler stores validated paper scenarios and dedupe state in SQLite, applies `BOT_SIGNAL_ALERT_COOLDOWN_SECONDS`, and checks completed-candle timestamps; restart does not reset alert history. `BOT_ALLOW_DUPLICATE_SIGNAL_ALERTS=true` permits same-state alerts on distinct candles but never resends the same dedupe key. Current XAU M5 provider support is absent, therefore no automatic signal alert can be emitted with this provider.

News polling reads dated headline/source/link metadata only from CoinDesk, Federal Reserve RSS feeds, and the BLS Latest Numbers feed. Automatic alerts are limited to XAU/USD-relevant HIGH and MEDIUM headlines. HIGH includes direct CPI/FOMC/payroll/rate decisions and major geopolitical developments; MEDIUM covers relevant gold, dollar, yield, inflation, and Fed reporting; LOW remains visible in `/news` without automatic delivery. A first successful poll records a SQLite baseline without sending old stories. Story IDs and URLs are persisted; unchanged headlines are suppressed. A late item whose publication/update time predates the previous poll is recorded without alerting, preventing feed backfill from generating a backlog. Explicit RSS/Atom update timestamps are retained separately from original publication dates; a large headline change is eligible as a substantial update only when its publisher activity timestamp is newer than the previous poll. Each allowlisted user can choose their own settings with `/news_on`, `/news_off`, `/news_high`, and `/news_all`; `/news_status` reports them and `/news_test` checks the Telegram reply path with a diagnostic clearly marked as not real news. `BOT_NEWS_ALERTS_PER_HOUR` defaults to six per authorized user. `BOT_NEWS_GEMINI_SUMMARIES_PER_HOUR` defaults to six automatic summary requests per hour across the bot; attempted calls, including quota failures, are counted in SQLite. Gemini explanations are cached locally for 30 minutes, and cached explanations do not consume another request slot. If Gemini is missing, rate-limited, out of quota, over the local hourly cap, or returns an invalid response, the alert still contains the sourced publisher headline, timestamps, and original URL with an explicitly uncertain potential impact. Feed headlines are untrusted data and are never treated as prompt instructions. Telegram remains restricted to private allowlisted recipients, messages are paced, 429 delays use `retry_after`, and transient errors use bounded backoff. Telegram has no send idempotency key, so a network loss after remote acceptance can still create an ambiguous retry.

`/status` reports scheduler state, configured signal market/timeframe/mode, timezone, check intervals, last signal-data status and completed-candle timestamp, alert queue health, sanitized last error, and SQLite availability. It does not probe every provider on demand; current provider availability appears after the next scheduled check. Gemini is optional for manual explanation and is not called to decide or generate automatic alerts.

The configured Gold API free OHLC path has a 10 requests/hour shared limit for history/OHLC and only returns the requested-range aggregate used by this client; Gold API's FAQ lists minute/hour groupings as a Premium feature. Therefore, it does not provide verified authentic XAU/USD M5 candles here. The bot explicitly displays `DATA UNAVAILABLE`, never switches to spot or another timeframe, and sends no automatic gold paper-signal alerts until the provider contract changes or a permitted free provider is configured. No paid service has been added.

## Local macOS operation

For Oracle Cloud Ubuntu ARM64 setup, the protected systemd unit, target-native CMake verification, health check, upgrade steps, and Always Free limits are documented in [DEPLOY.md](DEPLOY.md).

The Mac must be awake, logged in, and connected to the internet for continuous alerts. For a foreground session, run `./scripts/run.sh` from the project directory and leave that terminal running. Use `Control-C` to stop cleanly. For user-session startup and automatic process restart after an exit, build and configure `.env` first, then generate and load a LaunchAgent:

```sh
chmod 600 .env
./scripts/build.sh
./scripts/install_launch_agent.sh
launchctl bootstrap "gui/$(id -u)" "$HOME/Library/LaunchAgents/com.local.gemini-telegram-trading-bot.plist"
launchctl print "gui/$(id -u)/com.local.gemini-telegram-trading-bot"
```

Restart the loaded agent with `launchctl kickstart -k "gui/$(id -u)/com.local.gemini-telegram-trading-bot"`. Stop and unload it with `launchctl bootout "gui/$(id -u)/com.local.gemini-telegram-trading-bot"`. The LaunchAgent contains only paths and restart settings; credentials remain in the ignored, owner-only `.env` file. Standard output and errors go to `~/Library/Logs/GeminiTelegramTradingBot/`. LaunchAgents run in the logged-in user's session; sleep, logout, power loss, or a network outage pauses monitoring. Launchd restarts the process after exit, while the app itself retries transient Telegram connection failures. Apple documents LaunchAgent registration and lifecycle in its [Service Management documentation](https://developer.apple.com/documentation/servicemanagement/smappservice). No cloud host is involved, so the development/hosting target remains $0/month; provider free-tier availability and Telegram connectivity are external dependencies.

The configured timezone must be installed on macOS (for example `Asia/Phnom_Penh` or `America/Los_Angeles`). The main process applies it before starting the scheduler. On first launch, market/news checks run immediately; restarts honor persisted next-check times. During the first successful news poll after each process start, headlines published before the process started are recorded in SQLite without backlog alerts.

## Tests and deployment limits

`./scripts/test.sh` builds the project and runs all deterministic tests. `./scripts/integration-test.sh` runs mocked scheduler integration checks; it performs no live API requests. The Linux ARM64 deployment steps and target-native build instructions are in [DEPLOY.md](DEPLOY.md); an ARM64 deployment must be verified on the target VM.

Continuous alerts depend on the Mac staying powered, awake, logged in, and online. Free provider limits and RSS changes may interrupt status/news. RSS feed publication times can lag the underlying event, and impact classes are deterministic headline triage rather than a measure of market reaction. With the current Gold API candle granularity, no M5 gold setup or alert can be confirmed. No real-money execution or brokerage connection exists.

## Project layout

```text
include/trading_bot/   Public C++ interfaces, including technical, signal, and backtest engines
src/                   C++20 implementations, market providers, RSS, Gemini, simulations, and executable
tests/                 Unit tests with mocked Telegram, RSS, Gemini, and market responses
```
