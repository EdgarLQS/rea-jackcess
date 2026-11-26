#!/usr/bin/env bash
# Builds and tests the project the way CI does.
#
# Runs on Ubuntu runners and inside an arm64 container. Local use:
#   SKIP_APT=1 bash .github/scripts/ci.sh
#
# Environment:
#   BUILD_DIR       build directory (default: build)
#   FIXTURE_DIR     generated fixture directory (default: $BUILD_DIR/fixtures)
#   JAR_CACHE       where the fixed JARs are stored (default: /tmp/rea-ci-jars)
#   CTEST_EXCLUDE   regex of tests to skip (default: none)
#   SANITIZE        set to 1 for an ASan+UBSan build
#   SKIP_APT        set to 1 to skip package installation (local macOS/Linux runs)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
FIXTURE_DIR="${FIXTURE_DIR:-$BUILD_DIR/fixtures}"
JAR_CACHE="${JAR_CACHE:-/tmp/rea-ci-jars}"
JACKCESS_VERSION="5.2.0"
JACKCESS_SHA256="9451d6f336cfaa82f5af178d5f6668b933cda4f27e261f29a1faa83630f95775"
LANG3_SHA256="7b96bf3ee68949abb5bc465559ac270e0551596fa34523fddf890ec418dde13c"
COMMONS_LOGGING_SHA256="daddea1ea0be0f56978ab3006b8ac92834afeefbd9b7e4e6316fca57df0fa636"

if [ "$(id -u)" = "0" ]; then
    SUDO=""
else
    SUDO="sudo"
fi

if [ "${SKIP_APT:-0}" != "1" ]; then
    if ! command -v apt-get >/dev/null 2>&1; then
        echo "SKIP_APT=1 is required off Debian/Ubuntu hosts" >&2
        exit 2
    fi
    $SUDO apt-get update
    # zlib1g-dev is required by find_package(ZLIB REQUIRED) in the top-level CMakeLists.
    $SUDO apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build default-jdk-headless curl python3 ca-certificates zlib1g-dev
fi

mkdir -p "$JAR_CACHE"
base="https://repo1.maven.org/maven2"
curl -fsSL -o "$JAR_CACHE/jackcess-$JACKCESS_VERSION.jar" \
    "$base/io/github/spannm/jackcess/$JACKCESS_VERSION/jackcess-$JACKCESS_VERSION.jar"
curl -fsSL -o "$JAR_CACHE/commons-lang3-3.14.0.jar" \
    "$base/org/apache/commons/commons-lang3/3.14.0/commons-lang3-3.14.0.jar"
curl -fsSL -o "$JAR_CACHE/commons-logging-1.2.jar" \
    "$base/commons-logging/commons-logging/1.2/commons-logging-1.2.jar"
if command -v sha256sum >/dev/null 2>&1; then
    check_hash() { echo "$1  $2" | sha256sum -c -; }
else
    check_hash() { echo "$1  $2" | shasum -a 256 -c -; }
fi
check_hash "$JACKCESS_SHA256" "$JAR_CACHE/jackcess-$JACKCESS_VERSION.jar"
check_hash "$LANG3_SHA256" "$JAR_CACHE/commons-lang3-3.14.0.jar"
check_hash "$COMMONS_LOGGING_SHA256" "$JAR_CACHE/commons-logging-1.2.jar"

jars="$JAR_CACHE/*"
classes="$JAR_CACHE/classes"
mkdir -p "$classes" "$FIXTURE_DIR"
javac -cp "$jars" -d "$classes" \
    tools/validation/JackcessJet4DataFixture.java \
    tools/validation/JackcessJet4IndexFixture.java \
    tools/validation/JackcessAceFixture.java
java -cp "$classes:$jars" JackcessJet4DataFixture "$FIXTURE_DIR/jet4-data-v2000.mdb" V2000
java -cp "$classes:$jars" JackcessJet4DataFixture "$FIXTURE_DIR/jet4-data-v2003.mdb" V2003
java -cp "$classes:$jars" JackcessJet4IndexFixture "$FIXTURE_DIR/jet4-index-v2000.mdb" V2000
java -cp "$classes:$jars" JackcessJet4IndexFixture "$FIXTURE_DIR/jet4-index-v2003.mdb" V2003
java -cp "$classes:$jars" JackcessAceFixture "$FIXTURE_DIR/ace-v2016.accdb" V2016
java -cp "$classes:$jars" JackcessAceFixture "$FIXTURE_DIR/ace-v2019.accdb" V2019

case "$FIXTURE_DIR" in
    /*) FIXTURE_ABS="$FIXTURE_DIR" ;;
    *) FIXTURE_ABS="$ROOT/$FIXTURE_DIR" ;;
esac

cmake_args=(-S . -B "$BUILD_DIR" -G Ninja -DREA_JACKCESS_FIXTURE_DIR="$FIXTURE_ABS")
if [ "${SANITIZE:-0}" = "1" ]; then
    cmake_args+=(
        -DCMAKE_BUILD_TYPE=Debug
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
    )
else
    cmake_args+=(-DCMAKE_BUILD_TYPE=RelWithDebInfo)
fi
cmake "${cmake_args[@]}"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"
cmake --build "$BUILD_DIR" -j"$JOBS"

ctest_args=(--test-dir "$BUILD_DIR" --output-on-failure)
if [ -n "${CTEST_EXCLUDE:-}" ]; then
    ctest_args+=(-E "$CTEST_EXCLUDE")
fi
ctest "${ctest_args[@]}"
