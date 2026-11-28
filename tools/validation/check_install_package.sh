#!/usr/bin/env bash
# Installs an existing build tree and configures a downstream CMake project against it.
#
# Usage: tools/validation/check_install_package.sh [build-dir] [prefix]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-$ROOT/build}"
PREFIX="${2:-$ROOT/build/package-prefix}"
CONSUMER_SOURCE="$ROOT/tools/validation/package-consumer"
CONSUMER_BUILD="$ROOT/build/package-consumer-build"

rm -rf "$PREFIX" "$CONSUMER_BUILD"
cmake --install "$BUILD_DIR" --prefix "$PREFIX"
cmake -S "$CONSUMER_SOURCE" -B "$CONSUMER_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$PREFIX"
cmake --build "$CONSUMER_BUILD"
ctest --test-dir "$CONSUMER_BUILD" --output-on-failure
