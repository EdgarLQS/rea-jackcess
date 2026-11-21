#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace rea_jackcess {

struct DecimalValue {
    // Decimal text for MONEY writes; conversion to four fractional digits is exact and does not round.
    std::string text;
};

struct DateTimeValue {
    std::string iso8601;
};

struct DateOutOfRangeValue {
    double serial;
};

struct GuidValue {
    // Canonical 8-4-4-4-12 hexadecimal text; Jet4 writes also accept surrounding braces.
    std::string text;
};

struct MemoValue {
    // Writes use non-empty raw_bytes verbatim; otherwise text is encoded as Jet4 UTF-16LE.
    // An empty MemoValue is distinct from NULL (std::monostate).
    std::string text;
    std::vector<std::uint8_t> raw_bytes;
};

struct BlobValue {
    std::vector<std::uint8_t> bytes;
};

struct UnsupportedValue {
    std::string description;
};

// Jet4 TEXT values use UTF-8 std::string values; BINARY values use BlobValue.
using Value = std::variant<std::monostate,
                           bool,
                           std::int8_t,
                           std::int16_t,
                           std::int32_t,
                           std::int64_t,
                           float,
                           double,
                           std::string,
                           DecimalValue,
                           DateTimeValue,
                           DateOutOfRangeValue,
                           GuidValue,
                           MemoValue,
                           BlobValue,
                           UnsupportedValue>;

}
