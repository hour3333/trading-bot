FROM debian:bookworm-slim AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        libcurl4-openssl-dev \
        libsqlite3-dev \
        libxml2-dev \
        nlohmann-json3-dev \
        ninja-build \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
    && cmake --build /build --parallel 2

FROM debian:bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        gosu \
        libcurl4 \
        libsqlite3-0 \
        libxml2 \
        tzdata \
    && useradd --system --uid 10001 --create-home tradingbot \
    && mkdir -p /data \
    && chown tradingbot:tradingbot /data \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /build/trading_bot /usr/local/bin/trading_bot
COPY deploy/railway-entrypoint.sh /usr/local/bin/railway-entrypoint.sh
RUN chmod 0755 /usr/local/bin/railway-entrypoint.sh

WORKDIR /app
ENV BOT_DATABASE_PATH=/data/trading_bot.sqlite3
ENTRYPOINT ["/usr/local/bin/railway-entrypoint.sh"]
