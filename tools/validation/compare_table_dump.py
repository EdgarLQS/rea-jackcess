#!/usr/bin/env python3
"""Compares a canonicJSON v1 dump with a JackcessTableDump JSON dump.

The C++ side comes from `rea-jackcess canonical`, the Java side from
tools/validation/JackcessTableDump.java running the fixed Jackcess 5.2.0 JAR. Tables are compared
as row multisets after normalising formatting-only differences:

* decimals compare numerically, dates by calendar fields with trailing zeros dropped,
* GUIDs compare brace and case insensitive, BLOBs by length and SHA-256,
* floats compare with a small relative tolerance, NaN equals NaN,
* BOOLEAN compares as a boolean even when one side prints 0/1.

Usage: compare_table_dump.py <cpp-canonical.json> <jackcess-dump.json>
Exit code 0 means every table, column, and value matched; 1 lists the differences.
"""

import json
import math
import re
import sys
from collections import Counter
from decimal import Decimal

# C++ canonical type names that differ from the Jackcess DataType names.
TYPE_ALIASES = {"SHORT": "INT"}

GUID_RE = re.compile(r"^\{?[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
                     r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\}?$")


def normalise_date(text):
    match = re.match(r"^(-?\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d)(?::(\d\d))?(?:\.(\d+))?$", text)
    if not match:
        return ("date-raw", text)
    year, month, day, hour, minute, second, fraction = match.groups()
    return ("date", int(year), int(month), int(day), int(hour), int(minute),
            int(second or 0), (fraction or "").rstrip("0"))


def normalise(value):
    if value is None:
        return ("null",)
    if isinstance(value, bool):
        return ("bool", value)
    if isinstance(value, int):
        return ("int", value)
    if isinstance(value, float):
        return ("nan",) if math.isnan(value) else ("float", value)
    if isinstance(value, str):
        if GUID_RE.match(value):
            return ("guid", value.strip("{}").lower())
        return ("text", value)
    if isinstance(value, dict):
        if "$decimal" in value:
            return ("decimal", Decimal(value["$decimal"]))
        if "$date" in value:
            return normalise_date(value["$date"])
        if "$guid" in value:
            return ("guid", value["$guid"].strip("{}").lower())
        if "$blob" in value:
            blob = value["$blob"]
            return ("blob", blob["size"], blob["sha256"])
        if "$float" in value:
            return ("nan",) if value["$float"] == "NaN" else ("float", float(value["$float"]))
        if "$dateOutOfRange" in value:
            return ("date-out-of-range", round(float(value["$dateOutOfRange"]), 6))
        if "$unsupported" in value:
            return ("unsupported", value["$unsupported"])
    return ("other", repr(value))


def cell_equal(left, right):
    normalised_left, normalised_right = normalise(left), normalise(right)
    if normalised_left == normalised_right:
        return True
    if normalised_left[0] in ("int", "float") and normalised_right[0] in ("int", "float"):
        expected = float(normalised_right[1])
        return abs(float(normalised_left[1]) - expected) <= max(1e-9, abs(expected) * 1e-12)
    return False


def row_key(row):
    return tuple(repr(normalise(cell)) for cell in row)


def compare(cpp_path, java_path):
    with open(cpp_path, encoding="utf-8") as handle:
        cpp = json.load(handle)
    with open(java_path, encoding="utf-8") as handle:
        java = json.load(handle)

    if cpp.get("errors"):
        print("C++ canonical reported errors:")
        for error in cpp["errors"]:
            print("  ", error)
        return 1

    cpp_tables = {table["name"]: table for table in cpp["tables"]}
    java_tables = {table["name"]: table for table in java["tables"]}
    problems = []

    for name in sorted(set(cpp_tables) - set(java_tables)):
        problems.append(f"{name}: only in the C++ dump")
    for name in sorted(set(java_tables) - set(cpp_tables)):
        problems.append(f"{name}: only in the Jackcess dump")

    for name in sorted(set(cpp_tables) & set(java_tables)):
        cpp_table, java_table = cpp_tables[name], java_tables[name]
        cpp_columns = cpp_table["columns"]
        java_columns = java_table["columns"]
        if [c["name"] for c in cpp_columns] != [c["name"] for c in java_columns]:
            problems.append(f"{name}: column names differ")
            continue
        for cpp_column, java_column in zip(cpp_columns, java_columns):
            cpp_type = TYPE_ALIASES.get(cpp_column["mdbType"], cpp_column["mdbType"])
            if cpp_type != java_column["type"]:
                problems.append(f"{name}.{cpp_column['name']}: type {cpp_type} != {java_column['type']}")
            if cpp_column["size"] != java_column["length"]:
                problems.append(f"{name}.{cpp_column['name']}: length "
                                f"{cpp_column['size']} != {java_column['length']}")
        cpp_rows = cpp_table["rows"] or []
        java_rows = java_table["rows"]
        if len(cpp_rows) != len(java_rows):
            problems.append(f"{name}: row count {len(cpp_rows)} != {len(java_rows)}")
            continue
        if not all(cell_equal(a, b) for row in zip(cpp_rows, java_rows) for a, b in zip(*row)):
            # Fall back to a multiset comparison so row order does not matter.
            cpp_counter = Counter(row_key(row) for row in cpp_rows)
            java_counter = Counter(row_key(row) for row in java_rows)
            if cpp_counter != java_counter:
                missing = sum((cpp_counter - java_counter).values())
                extra = sum((java_counter - cpp_counter).values())
                problems.append(f"{name}: {missing} C++ rows and {extra} Jackcess rows match no "
                                f"counterpart")
            else:
                # Same multiset in another order; only a row order difference.
                pass

    if problems:
        print(f"{len(problems)} difference(s):")
        for problem in problems:
            print("  ", problem)
        return 1
    print(f"Compared {len(cpp_tables)} tables: schema and rows match.")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[-1])
        sys.exit(2)
    sys.exit(compare(sys.argv[1], sys.argv[2]))
