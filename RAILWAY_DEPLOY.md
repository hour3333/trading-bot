# Railway deployment

This repository is configured as a single long-running C++20 worker. Railway
automatically detects the root `Dockerfile`; the image compiles the bot with
CMake and installs only its runtime libraries. No inbound port or public
domain is required because Telegram uses `getUpdates` long polling.

## Connect the existing Railway service

In the `trading-bot` service in the `production` environment:

1. Keep the GitHub source on `main`, auto-deploy enabled, and the replica count
   at **1**. Do not enable Serverless or a cron schedule for this worker.
2. Keep public networking disabled. Set the restart policy to **On Failure**
   with at most 10 retries.
3. Attach one Railway Volume at `/data`. The container entrypoint fixes the
   database path to `/data/trading_bot.sqlite3` and fixes the mounted directory
   ownership before dropping privileges, so SQLite can write safely even if
   an old relative `BOT_DATABASE_PATH` service variable is present.
4. Keep credentials in Railway's masked service Variables. Required for the
   Telegram worker are `TELEGRAM_BOT_TOKEN` and
   `TELEGRAM_ALLOWED_USER_IDS`. Optional provider variables are
   `GEMINI_API_KEY`, `COINGECKO_DEMO_API_KEY`, and `GOLD_API_KEY`. Set
   `BOT_TRADING_MODE=PAPER_ONLY` and `BOT_TIMEZONE=Asia/Phnom_Penh`.
5. Do not add API keys to GitHub, Docker build arguments, image layers, or
   Railway build logs. The Docker build context excludes `.env` and local data.
   If any credential was previously pasted into a chat or committed, rotate it
   at its provider, then enter the replacement directly in Railway Variables.
6. Use the Railway deployment's **Deploy Logs** and **Metrics** tabs to verify
   startup, ongoing polling, and resource usage. From the allowlisted Telegram
   account, `/status` checks the bot and `/news_status` checks news polling.

Variable names and safe baseline values are documented in `.env.example`.
Values configured in Railway take precedence over Docker defaults.

## Persistence and operating limits

The worker stores caches, update offsets, user preferences, paper signals, and
simulated trade history in SQLite under `/data`. A Railway volume is necessary;
without it the database is ephemeral. Keep one replica to avoid competing
Telegram `getUpdates` pollers.

Railway's Free plan currently has a `$0/month` base price and `$1/month` of
included resource credit. A Free/Trial volume is limited to 0.5 GB; volume
storage is usage-metered, and the current published rate is `$0.15/GB-month`.
Free service limits are 1 replica, 0.5 GB RAM, and 1 vCPU. This bot is a
continuously running worker, so actual RAM/CPU usage must be checked in Railway Metrics. Do
not upgrade the workspace or raise usage limits if the `$0/month` requirement
still applies. If included credit is exhausted, Railway may stop the service;
the volume is subject to Railway's Free/Trial retention policy. See Railway's
[pricing plans](https://docs.railway.com/pricing/plans),
[trial terms](https://docs.railway.com/pricing/free-trial), and
[volume reference](https://docs.railway.com/volumes/reference) for current
limits and retention.

The currently configured free Gold source does not provide verified M5 OHLC
candles. Gold M5 analysis and paper signals therefore remain `DATA UNAVAILABLE`
until a supported genuine-candle source is configured. The bot must not infer
candles from spot prices. All signal and backtest functionality is simulated;
the project has no brokerage integration or live-trading path.
