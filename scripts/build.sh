#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$PROJECT_ROOT/build"}

if command -v brew >/dev/null 2>&1; then
    BREW_PREFIX=$(brew --prefix)
    CMAKE_PREFIX_PATH="$BREW_PREFIX/opt/curl:$BREW_PREFIX/opt/nlohmann-json:$BREW_PREFIX/opt/sqlite:$BREW_PREFIX/opt/libxml2${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
    export CMAKE_PREFIX_PATH
fi

cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
cmake --build "$BUILD_DIR" --parallel "${BUILD_JOBS:-4}"
