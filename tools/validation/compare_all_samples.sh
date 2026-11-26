#!/usr/bin/env bash
# Compares the C++ canonical dump with the fixed Jackcess 5.2.0 dump for every database given.
#
# Usage: compare_all_samples.sh <rea-jackcess-binary> <database> [database...]
#
# Environment:
#   JACKCESS_CLASSPATH  classpath holding jackcess-5.2.0.jar and its two dependencies
#                       (default: /tmp/rea-ci-jars/*)
#   WORK_DIR            scratch directory (default: a fresh mktemp -d)
#
# Each file must match table for table: column names, types, lengths, and rows. ACE flat tables
# (f_ prefix) are listed only by the C++ dump and are not a difference. A missing JVM or JAR is a
# hard error, so this never reports a silent pass.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLI="${1:?usage: compare_all_samples.sh <rea-jackcess-binary> <database> [database...]}"
shift
if [ "$#" -eq 0 ]; then
    echo "usage: compare_all_samples.sh <rea-jackcess-binary> <database> [database...]" >&2
    exit 2
fi

CLASSES="${WORK_DIR:-$(mktemp -d)}/classes"
mkdir -p "$CLASSES"
CP="${JACKCESS_CLASSPATH:-/tmp/rea-ci-jars/*}"

javac -encoding UTF-8 -cp "$CP" -d "$CLASSES" \
    "$ROOT/tools/validation/JackcessTableDump.java" >/dev/null

failures=0
for database in "$@"; do
    name="$(basename "$database")"
    base="${CLASSES}/${name}"
    if ! "$CLI" canonical "$database" > "$base.cpp.json" 2> "$base.cpp.err"; then
        echo "$name: C++ canonical failed"
        sed -n 1,3p "$base.cpp.err"
        failures=$((failures + 1))
        continue
    fi
    if ! java -cp "$CLASSES:$CP" JackcessTableDump "$database" "$base.jar.json" >/dev/null; then
        echo "$name: Jackcess dump failed"
        failures=$((failures + 1))
        continue
    fi
    if python3 "$ROOT/tools/validation/compare_table_dump.py" "$base.cpp.json" "$base.jar.json"; then
        echo "$name: MATCH"
    else
        echo "$name: DIFF"
        failures=$((failures + 1))
    fi
done

if [ "$failures" -ne 0 ]; then
    echo "$failures sample(s) differ" >&2
    exit 1
fi
echo "All samples match the fixed Jackcess 5.2.0 dump."
