#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace rea_jackcess {

// Exact decimal text for a MONEY column. Parsing, writing and reading never go through binary
// floating point, so an amount such as "12345.6789" round-trips without rounding.
struct DecimalValue {
    // Decimal string (optional sign, digits, optional fraction). MONEY is stored as a 64-bit
    // scaled integer with four implied fractional digits, so text finer than 10^-4 is rejected.
    std::string text;
};

// A date/time column that decoded successfully.
struct DateTimeValue {
    // ISO-8601 local date-time text identical to Java's LocalDateTime.toString(), e.g.
    // "2024-01-31T15:04" (a zero seconds field and zero fraction are omitted) or
    // "2024-01-31T15:04:05.123". Decoded from a Jet4 DATE (double serial) or an ACE EXT_DATE_TIME.
    std::string iso8601;
};

// A DATE (double serial) whose value cannot be represented as an ISO-8601 date-time, i.e. it is
// non-finite or outside Access' valid serial range. The raw serial is preserved for callers that
// want to handle it themselves.
struct DateOutOfRangeValue {
    // The stored Jet4 date serial (days since 1899-12-30, fraction is the time of day).
    double serial;
};

// A GUID column value.
struct GuidValue {
    // Canonical 8-4-4-4-12 hexadecimal text. Reads always produce a lower-case braced form such as
    // "{00112233-4455-6677-8899-aabbccddeeff}"; Jet4 writes also accept the unbraced form.
    std::string text;
};

// A MEMO (long text) column value.
struct MemoValue {
    // Text form. On write, if raw_bytes is empty this is encoded as Jet4 UTF-16LE; an empty
    // MemoValue is an empty string, which is distinct from NULL (std::monostate).
    std::string text;
    // Raw stored payload, if the caller has it. On write a non-empty raw_bytes is used verbatim
    // and `text` is ignored, so the original bytes survive a round trip.
    std::vector<std::uint8_t> raw_bytes;
};

// A BINARY column value.
struct BlobValue {
    // Raw column bytes (a NULL or zero-length column is not a BlobValue; see the type of the cell).
    std::vector<std::uint8_t> bytes;
};

// A column value the decoder could not interpret (for example an unknown field type or a
// malformed date serial). It is never silently dropped: the raw bytes are retained.
struct UnsupportedValue {
    // Human-readable reason, e.g. "date serial is not finite".
    std::string description;
    // Encoded column bytes retained when the decoder does not understand the field type.
    std::vector<std::uint8_t> raw_bytes;
};

// One decoded cell. Which alternative is used depends on the column's DataType:
//   std::monostate             -> NULL
//   bool, int8/16/32/64, float, double -> BOOLEAN, BYTE, SHORT, LONG, INT64, SINGLE, DOUBLE
//   std::string                -> TEXT (UTF-8)
//   DecimalValue               -> MONEY / NUMERIC
//   DateTimeValue              -> DATE / EXT_DATE_TIME
//   DateOutOfRangeValue        -> a DATE serial outside the representable range
//   GuidValue                  -> GUID
//   MemoValue                  -> MEMO
//   BlobValue                  -> BINARY / OLE
//   UnsupportedValue           -> a type or value the decoder rejects
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
