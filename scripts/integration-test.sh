#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
"$SCRIPT_DIR/build.sh"
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$PROJECT_ROOT/build"}
ctest --test-dir "$BUILD_DIR" --output-on-failure -R '^phase8_scheduler_integration$'
