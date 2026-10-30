#!/bin/sh
set -eu

mkdir -p /data
chown tradingbot:tradingbot /data
export BOT_DATABASE_PATH=/data/trading_bot.sqlite3
exec gosu tradingbot /usr/local/bin/trading_bot "$@"
