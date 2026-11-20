#include "jet4_value.hpp"

#include "../util/little_endian.hpp"
#include "../page/data_page.hpp"
#include "text_codec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint32_t long_value_type_mask = 0xc0000000u;
constexpr std::uint32_t long_value_length_mask = 0x3fffffffu;
constexpr std::uint32_t long_value_inline = 0x80000000u;
constexpr std::uint32_t long_value_other_page = 0x40000000u;
constexpr std::size_t long_value_definition_size = 12;
constexpr std::size_t long_value_page_pointer_size = 4;
constexpr std::uint8_t boolean_type = 0x01;
constexpr std::uint8_t byte_type = 0x02;
constexpr std::uint8_t short_type = 0x03;
constexpr std::uint8_t long_type = 0x04;
constexpr std::uint8_t money_type = 0x05;
constexpr std::uint8_t float_type = 0x06;
constexpr std::uint8_t double_type = 0x07;
constexpr std::uint8_t date_type = 0x08;
constexpr std::uint8_t binary_type = 0x09;
constexpr std::uint8_t text_type = 0x0a;
constexpr std::uint8_t ole_type = 0x0b;
constexpr std::uint8_t memo_type = 0x0c;
constexpr std::uint8_t guid_type = 0x0f;
constexpr std::uint8_t numeric_type = 0x10;
constexpr std::uint8_t big_binary_type = 0x11;
constexpr std::uint8_t big_int_type = 0x13;
constexpr std::uint8_t extended_date_time_type = 0x14;

[[noreturn]] void value_error(const PageFile& file,
                              const std::string& table_name,
                              const EncodedTableRow& row,
                              const ColumnDefinition& column,
                              const char* message,
                              std::optional<std::uint64_t> page_number = std::nullopt) {
    DatabaseErrorContext context;
    context.table_name = table_name;
    context.row_number = row.row_number;
    context.column_number = column.column_number;
    context.column_name = column.name;
    throw DatabaseError(DatabaseErrorCode::corrupted_file,
                        message,
                        file.path(),
                        page_number ? page_number : std::optional<std::uint64_t>(row.page_number),
                        std::move(context));
}

DataPage read_value_page(const PageFile& file,
                         const std::string& table_name,
                         const EncodedTableRow& row,
                         const ColumnDefinition& column,
                         std::uint32_t page_number) {
    try {
        return read_data_page(file, page_number);
    } catch (const DatabaseError& error) {
        DatabaseErrorContext context;
        context.table_name = table_name;
        context.row_number = row.row_number;
        context.column_number = column.column_number;
        context.column_name = column.name;
        throw DatabaseError(error.code(), error.what(), file.path(), error.page_number(), std::move(context));
    }
}

std::vector<std::uint8_t> read_long_value(const PageFile& file,
                                          const std::string& table_name,
                                          const EncodedTableRow& row,
                                          const ColumnDefinition& column,
                                          const std::vector<std::uint8_t>& definition) {
    if (definition.size() < 4) {
        value_error(file, table_name, row, column, "Long-value definition is shorter than its type header");
    }
    const std::uint32_t length_with_flags = read_u32(definition, 0);
    const std::uint32_t value_type = length_with_flags & long_value_type_mask;
    const std::uint32_t length = length_with_flags & long_value_length_mask;
    const std::uint64_t maximum_database_bytes = file.page_count() * file.format().page_size;
    if (length > maximum_database_bytes) {
        value_error(file, table_name, row, column, "Long-value length exceeds the database size");
    }

    if (value_type == long_value_inline) {
        if (definition.size() < long_value_definition_size ||
            length > definition.size() - long_value_definition_size) {
            value_error(file, table_name, row, column, "Inline long-value data is truncated");
        }
        return {definition.begin() + static_cast<std::ptrdiff_t>(long_value_definition_size),
                definition.begin() + static_cast<std::ptrdiff_t>(long_value_definition_size + length)};
    }

    if (definition.size() != long_value_definition_size) {
        value_error(file, table_name, row, column, "External long-value definition has an invalid length");
    }
    std::uint16_t row_number = definition[4];
    std::uint32_t page_number = static_cast<std::uint32_t>(definition[5]) |
                                (static_cast<std::uint32_t>(definition[6]) << 8) |
                                (static_cast<std::uint32_t>(definition[7]) << 16);
    if (page_number == 0 || page_number >= file.page_count()) {
        value_error(file, table_name, row, column, "Long-value pointer references an invalid page", page_number);
    }

    if (value_type == long_value_other_page) {
        const DataPage page = read_value_page(file, table_name, row, column, page_number);
        if (row_number >= page.rows.size() || page.rows[row_number].deleted) {
            value_error(file, table_name, row, column, "Long-value pointer references an invalid row", page_number);
        }
        const auto& bytes = page.rows[row_number].bytes;
        if (bytes.size() < length) {
            value_error(file, table_name, row, column, "External long-value data is truncated", page_number);
        }
        return {bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length)};
    }
    if (value_type != 0) {
        value_error(file, table_name, row, column, "Long-value type flags are unsupported");
    }

    std::vector<std::uint8_t> result;
    result.reserve(length);
    std::set<std::pair<std::uint32_t, std::uint16_t>> visited;
    while (result.size() < length) {
        if (page_number == 0 || page_number >= file.page_count()) {
            value_error(file, table_name, row, column, "Long-value continuation references an invalid page", page_number);
        }
        if (!visited.emplace(page_number, row_number).second) {
            value_error(file, table_name, row, column, "Long-value continuation contains a cycle", page_number);
        }
        const DataPage page = read_value_page(file, table_name, row, column, page_number);
        if (row_number >= page.rows.size() || page.rows[row_number].deleted) {
            value_error(file, table_name, row, column, "Long-value continuation references an invalid row", page_number);
        }
        const auto& chunk = page.rows[row_number].bytes;
        if (chunk.size() < long_value_page_pointer_size) {
            value_error(file, table_name, row, column, "Long-value continuation pointer is truncated", page_number);
        }
        const std::uint16_t next_row = chunk[0];
        const std::uint32_t next_page = static_cast<std::uint32_t>(chunk[1]) |
                                        (static_cast<std::uint32_t>(chunk[2]) << 8) |
                                        (static_cast<std::uint32_t>(chunk[3]) << 16);
        const std::size_t remaining = static_cast<std::size_t>(length) - result.size();
        const std::size_t chunk_size = std::min(remaining, chunk.size() - long_value_page_pointer_size);
        result.insert(result.end(),
                      chunk.begin() + static_cast<std::ptrdiff_t>(long_value_page_pointer_size),
                      chunk.begin() + static_cast<std::ptrdiff_t>(long_value_page_pointer_size + chunk_size));
        if (result.size() == length) {
            break;
        }
        if (next_page == 0) {
            value_error(file, table_name, row, column, "Long-value continuation ended before its declared length", page_number);
        }
        row_number = next_row;
        page_number = next_page;
    }
    return result;
}

std::string decimal_text(std::uint64_t magnitude, bool negative, unsigned int scale) {
    std::string digits = std::to_string(magnitude);
    if (scale != 0) {
        if (digits.size() <= scale) {
            digits.insert(0, static_cast<std::size_t>(scale + 1 - digits.size()), '0');
        }
        digits.insert(digits.size() - scale, 1, '.');
    }
    if (negative && magnitude != 0) {
        digits.insert(digits.begin(), '-');
    }
    return digits;
}

std::string decimal_text_from_digits(std::string digits, bool negative, unsigned int scale) {
    if (scale != 0) {
        if (digits.size() <= scale) {
            digits.insert(0, static_cast<std::size_t>(scale + 1 - digits.size()), '0');
        }
        digits.insert(digits.size() - scale, 1, '.');
    }
    if (negative && digits.find_first_not_of("0.") != std::string::npos) {
        digits.insert(digits.begin(), '-');
    }
    return digits;
}

std::int64_t signed_value(std::uint64_t bits) {
    if (bits <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(bits);
    }
    return -1 - static_cast<std::int64_t>(~bits);
}

std::string money_text(std::int64_t value) {
    const std::uint64_t bits = static_cast<std::uint64_t>(value);
    const bool negative = value < 0;
    const std::uint64_t magnitude = negative ? (~bits + 1) : bits;
    return decimal_text(magnitude, negative, 4);
}

std::string guid_text(const std::vector<std::uint8_t>& bytes) {
    constexpr char hex[] = "0123456789abcdef";
    std::array<std::uint8_t, 16> ordered{};
    ordered[0] = bytes[3];
    ordered[1] = bytes[2];
    ordered[2] = bytes[1];
    ordered[3] = bytes[0];
    ordered[4] = bytes[5];
    ordered[5] = bytes[4];
    ordered[6] = bytes[7];
    ordered[7] = bytes[6];
    std::copy(bytes.begin() + 8, bytes.end(), ordered.begin() + 8);
    std::string result;
    result.reserve(38);
    result.push_back('{');
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            result.push_back('-');
        }
        result.push_back(hex[ordered[index] >> 4]);
        result.push_back(hex[ordered[index] & 0x0f]);
    }
    result.push_back('}');
    return result;
}

struct CivilDate {
    std::int64_t year;
    std::int64_t month;
    std::int64_t day;
};

CivilDate civil_from_days(std::int64_t days_since_epoch) {
    const std::int64_t shifted = days_since_epoch + 719468;
    const std::int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const std::int64_t day_of_era = shifted - era * 146097;
    const std::int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    std::int64_t year = year_of_era + era * 400;
    const std::int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const std::int64_t month_prime = (5 * day_of_year + 2) / 153;
    const std::int64_t day = day_of_year - (153 * month_prime + 2) / 5 + 1;
    const std::int64_t month = month_prime + (month_prime < 10 ? 3 : -9);
    year += month <= 2;
    return {year, month, day};
}

std::string format_datetime(std::int64_t unix_days, std::uint64_t nanoseconds_of_day) {
    constexpr std::uint64_t nanoseconds_per_hour = 3600000000000ull;
    constexpr std::uint64_t nanoseconds_per_minute = 60000000000ull;
    constexpr std::uint64_t nanoseconds_per_second = 1000000000ull;
    const CivilDate date = civil_from_days(unix_days);
    const std::uint64_t hour = nanoseconds_of_day / nanoseconds_per_hour;
    nanoseconds_of_day %= nanoseconds_per_hour;
    const std::uint64_t minute = nanoseconds_of_day / nanoseconds_per_minute;
    nanoseconds_of_day %= nanoseconds_per_minute;
    const std::uint64_t second = nanoseconds_of_day / nanoseconds_per_second;
    const std::uint64_t nanosecond = nanoseconds_of_day % nanoseconds_per_second;
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << date.year << '-'
           << std::setw(2) << date.month << '-' << std::setw(2) << date.day << 'T'
           << std::setw(2) << hour << ':' << std::setw(2) << minute << ':' << std::setw(2) << second;
    if (nanosecond != 0) {
        output << '.' << std::setw(9) << nanosecond;
        std::string result = output.str();
        while (result.back() == '0') {
            result.pop_back();
        }
        return result;
    }
    return output.str();
}

std::string datetime_text(double serial) {
    constexpr double maximum_access_serial = 2958465.9999999;
    if (!std::isfinite(serial) || serial < 0.0 || serial > maximum_access_serial) {
        return {};
    }
    constexpr std::int64_t milliseconds_per_day = 86400000;
    const std::int64_t whole_days = static_cast<std::int64_t>(std::trunc(serial));
    std::int64_t milliseconds = static_cast<std::int64_t>(
        std::llround(std::fmod(std::fabs(serial), 1.0) * milliseconds_per_day));
    std::int64_t access_days = whole_days;
    if (milliseconds >= milliseconds_per_day) {
        ++access_days;
        milliseconds -= milliseconds_per_day;
    }
    constexpr std::int64_t access_epoch_days_from_unix = -25569;
    const std::int64_t unix_days = access_days + access_epoch_days_from_unix;
    return format_datetime(unix_days, static_cast<std::uint64_t>(milliseconds) * 1000000ull);
}

std::uint64_t read_decimal_field(const std::vector<std::uint8_t>& bytes,
                                 std::size_t offset,
                                 std::size_t count) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint8_t character = bytes[offset + index];
        if (character < '0' || character > '9') {
            return std::numeric_limits<std::uint64_t>::max();
        }
        const std::uint64_t digit = character - '0';
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        value = value * 10 + digit;
    }
    return value;
}

std::optional<std::string> extended_datetime_text(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != 42 || bytes[19] != ':' || bytes[39] != ':' ||
        bytes[40] != '7' || bytes[41] != 0) {
        return std::nullopt;
    }
    const std::uint64_t days = read_decimal_field(bytes, 0, 19);
    const std::uint64_t seconds = read_decimal_field(bytes, 20, 12);
    const std::uint64_t nanos = read_decimal_field(bytes, 32, 7);
    if (days == std::numeric_limits<std::uint64_t>::max() ||
        seconds == std::numeric_limits<std::uint64_t>::max() ||
        nanos == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    constexpr std::uint64_t seconds_per_day = 86400;
    constexpr std::int64_t unix_days_at_year_one = -719162;
    if (days > std::numeric_limits<std::uint64_t>::max() - seconds / seconds_per_day) {
        return std::nullopt;
    }
    const std::uint64_t extra_days = days + seconds / seconds_per_day;
    constexpr std::uint64_t maximum_shiftable_days =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - 306);
    if (extra_days > maximum_shiftable_days) {
        return std::nullopt;
    }
    const std::int64_t unix_days = static_cast<std::int64_t>(extra_days) + unix_days_at_year_one;
    const std::uint64_t seconds_of_day = seconds % seconds_per_day;
    const std::uint64_t nanoseconds_of_day = seconds_of_day * 1000000000ull + nanos * 100ull;
    return format_datetime(unix_days, nanoseconds_of_day);
}

std::string unsigned_128_decimal(const std::vector<std::uint8_t>& big_endian) {
    std::string digits = "0";
    for (const std::uint8_t byte : big_endian) {
        unsigned int carry = byte;
        for (auto digit = digits.rbegin(); digit != digits.rend(); ++digit) {
            const unsigned int value = static_cast<unsigned int>(*digit - '0') * 256 + carry;
            *digit = static_cast<char>('0' + value % 10);
            carry = value / 10;
        }
        while (carry != 0) {
            digits.insert(digits.begin(), static_cast<char>('0' + carry % 10));
            carry /= 10;
        }
    }
    const auto first_nonzero = digits.find_first_not_of('0');
    return first_nonzero == std::string::npos ? "0" : digits.substr(first_nonzero);
}

std::string numeric_text(const std::vector<std::uint8_t>& bytes, unsigned int scale) {
    const bool negative = bytes[0] != 0;
    std::vector<std::uint8_t> big_endian(16);
    for (std::size_t group = 0; group < 4; ++group) {
        for (std::size_t index = 0; index < 4; ++index) {
            big_endian[group * 4 + index] = bytes[1 + group * 4 + (3 - index)];
        }
    }
    return decimal_text_from_digits(unsigned_128_decimal(big_endian), negative, scale);
}

}

Jet4Value decode_jet4_value(const PageFile& file,
                            const std::string& table_name,
                            const EncodedTableRow& row,
                            const ColumnDefinition& column) {
    if (column.column_number >= row.columns.size()) {
        value_error(file, table_name, row, column, "Decoded row does not contain the column");
    }
    const auto& encoded = row.columns[column.column_number];
    if (column.type == boolean_type) {
        return encoded.boolean_value;
    }
    if (!encoded.bytes) {
        return std::monostate{};
    }
    const auto& bytes = *encoded.bytes;

    switch (column.type) {
        case byte_type:
            if (bytes.size() != 1) {
                value_error(file, table_name, row, column, "Byte value has an invalid byte length");
            }
            return static_cast<std::int8_t>(bytes[0]);
        case short_type:
            if (bytes.size() != 2) {
                value_error(file, table_name, row, column, "16-bit integer has an invalid byte length");
            }
            {
                const std::uint16_t bits = read_u16(bytes, 0);
                if (bits <= static_cast<std::uint16_t>(std::numeric_limits<std::int16_t>::max())) {
                    return static_cast<std::int16_t>(bits);
                }
                return static_cast<std::int16_t>(-1 - static_cast<std::int32_t>(static_cast<std::uint16_t>(~bits)));
            }
        case long_type:
            if (bytes.size() != 4) {
                value_error(file, table_name, row, column, "32-bit integer has an invalid byte length");
            }
            {
                const std::uint32_t bits = read_u32(bytes, 0);
                if (bits <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
                    return static_cast<std::int32_t>(bits);
                }
                return static_cast<std::int32_t>(-1 - static_cast<std::int64_t>(~bits));
            }
        case big_int_type:
            if (bytes.size() != 8) {
                value_error(file, table_name, row, column, "64-bit integer has an invalid byte length");
            }
            return signed_value(read_u64(bytes, 0));
        case float_type: {
            if (bytes.size() != 4) {
                value_error(file, table_name, row, column, "Float32 value has an invalid byte length");
            }
            const std::uint32_t bits = read_u32(bytes, 0);
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }
        case double_type:
        case date_type: {
            if (bytes.size() != 8) {
                value_error(file, table_name, row, column, "Float64 value has an invalid byte length");
            }
            const std::uint64_t bits = read_u64(bytes, 0);
            double value;
            std::memcpy(&value, &bits, sizeof(value));
            if (column.type == date_type) {
                if (!std::isfinite(value)) {
                    return UnsupportedValue{"date serial is not finite"};
                }
                const auto text = datetime_text(value);
                if (text.empty()) {
                    return DateOutOfRangeValue{value};
                }
                return DateTimeValue{text};
            }
            return value;
        }
        case extended_date_time_type: {
            const auto text = extended_datetime_text(bytes);
            if (!text) {
                value_error(file, table_name, row, column, "Extended date/time value has an invalid encoding");
            }
            return DateTimeValue{*text};
        }
        case money_type:
            if (bytes.size() != 8) {
                value_error(file, table_name, row, column, "Money value has an invalid byte length");
            }
            return DecimalValue{money_text(signed_value(read_u64(bytes, 0)))};
        case numeric_type:
            if (bytes.size() != 17) {
                value_error(file, table_name, row, column, "Numeric value has an invalid byte length");
            }
            if (column.scale < 0) {
                value_error(file, table_name, row, column, "Numeric scale is negative");
            }
            return DecimalValue{numeric_text(bytes, column.scale)};
        case binary_type:
        case big_binary_type:
            return BlobValue{bytes};
        case ole_type: {
            if (bytes.empty()) {
                return std::monostate{};
            }
            return BlobValue{read_long_value(file, table_name, row, column, bytes)};
        }
        case memo_type: {
            if (bytes.empty()) {
                return std::monostate{};
            }
            const auto long_bytes = read_long_value(file, table_name, row, column, bytes);
            return MemoValue{decode_jet4_text(long_bytes), long_bytes};
        }
        case text_type:
            return decode_jet4_text(bytes);
        case guid_type:
            if (bytes.size() != 16) {
                value_error(file, table_name, row, column, "GUID value has an invalid byte length");
            }
            return GuidValue{guid_text(bytes)};
        default:
            return UnsupportedValue{"unsupported MDB type " + std::to_string(column.type)};
    }
}

}
