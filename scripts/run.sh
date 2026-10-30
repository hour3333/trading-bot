#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$PROJECT_ROOT/build"}

if [ ! -x "$BUILD_DIR/trading_bot" ]; then
    "$SCRIPT_DIR/build.sh"
fi

cd "$PROJECT_ROOT"
exec "$BUILD_DIR/trading_bot"
