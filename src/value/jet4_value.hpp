#pragma once

#include "../catalog/row_reader.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include "rea_jackcess/value.hpp"

namespace rea_jackcess::detail {

using DecimalValue = rea_jackcess::DecimalValue;
using DateTimeValue = rea_jackcess::DateTimeValue;
using DateOutOfRangeValue = rea_jackcess::DateOutOfRangeValue;
using GuidValue = rea_jackcess::GuidValue;
using MemoValue = rea_jackcess::MemoValue;
using BlobValue = rea_jackcess::BlobValue;
using UnsupportedValue = rea_jackcess::UnsupportedValue;
using Jet4Value = rea_jackcess::Value;

// Converts a decimal text to the Jet4 currency (MONEY) storage value: a signed 64-bit
// integer scaled by four decimal places. Throws std::invalid_argument when the text is not
// a decimal number, has more than four non-zero decimal places, or exceeds the range.
std::int64_t jet4_money_scaled_value(std::string_view text);

// Converts canonical UUID text (braces optional) to the 16 bytes Jet4 stores, i.e. the first
// three fields little-endian and the remaining bytes in order. Throws std::invalid_argument for
// malformed text.
std::array<std::uint8_t, 16> jet4_guid_bytes(std::string_view text);

// Parses the ISO_LOCAL_DATE_TIME text the reader produces into the Access date serial Jet4
// stores. The seconds field and fraction are optional and either 'T' or a space may separate the
// date and time, so a decoded value can be written back unchanged. Returns nullopt and fills
// `error` when the text is not a valid date inside the Access range.
std::optional<double> jet4_date_serial(std::string_view text, std::string* error = nullptr);

Jet4Value decode_jet4_value(const PageFile& file,
                            const std::string& table_name,
                            const EncodedTableRow& row,
                            const ColumnDefinition& column);

std::optional<std::int32_t> decode_jet4_complex_value_foreign_key(const PageFile& file,
                                                                  const std::string& table_name,
                                                                  const EncodedTableRow& row,
                                                                  const ColumnDefinition& column);

}
