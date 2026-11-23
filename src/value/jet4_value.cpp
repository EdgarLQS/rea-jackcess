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
constexpr std::uint8_t complex_type = 0x12;
constexpr std::uint8_t big_int_type = 0x13;
constexpr std::uint8_t extended_date_time_type = 0x14;
constexpr std::size_t calculated_value_length_offset = 16;
constexpr std::size_t calculated_value_data_offset = 20;

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
    // Java's LocalDateTime.toString() omits a zero seconds field and a zero fraction, and that
    // is the text Jackcess exposes and its ExportUtil writes, so the stored form repeats it.
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << date.year << '-'
           << std::setw(2) << date.month << '-' << std::setw(2) << date.day << 'T'
           << std::setw(2) << hour << ':' << std::setw(2) << minute;
    if (second == 0 && nanosecond == 0) {
        return output.str();
    }
    output << ':' << std::setw(2) << second;
    if (nanosecond != 0) {
        // ISO_LOCAL_DATE_TIME prints the fraction in groups of three digits, so .5 seconds is
        // rendered as .500 and one microsecond as .000001.
        const int digits = nanosecond % 1000000 == 0 ? 3 : (nanosecond % 1000 == 0 ? 6 : 9);
        std::string fraction = std::to_string(nanosecond + 1000000000ull).substr(1);
        fraction.resize(static_cast<std::size_t>(digits));
        return output.str() + "." + fraction;
    }
    return output.str();
}

std::string datetime_text(double serial) {
    constexpr double minimum_access_serial = -657435.0;
    constexpr double maximum_access_serial = 2958465.9999999;
    if (!std::isfinite(serial) || serial <= minimum_access_serial || serial > maximum_access_serial) {
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

std::vector<std::uint8_t> unwrap_calculated_value(const PageFile& file,
                                                  const std::string& table_name,
                                                  const EncodedTableRow& row,
                                                  const ColumnDefinition& column,
                                                  const std::vector<std::uint8_t>& stored_bytes) {
    if (stored_bytes.size() < calculated_value_data_offset) {
        return stored_bytes;
    }
    const auto declared_length = read_u32(stored_bytes, calculated_value_length_offset);
    if (declared_length > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        value_error(file, table_name, row, column, "Calculated value has a negative payload length");
    }
    const std::size_t available_length = stored_bytes.size() - calculated_value_data_offset;
    const std::size_t payload_length = std::min(available_length, static_cast<std::size_t>(declared_length));
    return {stored_bytes.begin() + static_cast<std::ptrdiff_t>(calculated_value_data_offset),
            stored_bytes.begin() + static_cast<std::ptrdiff_t>(calculated_value_data_offset + payload_length)};
}

}

std::optional<double> jet4_date_serial(std::string_view text, std::string* error) {
    const auto fail = [&](const char* reason) -> std::optional<double> {
        if (error != nullptr) {
            *error = reason;
        }
        return std::nullopt;
    };
    if (text.size() < 10 || text[4] != '-' || text[7] != '-') {
        return fail("date must use the YYYY-MM-DDTHH:MM form");
    }
    const auto read_number = [&](std::size_t offset, std::size_t count, const char* reason) {
        int value = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const char character = text[offset + index];
            if (character < '0' || character > '9') {
                fail(reason);
                return -1;
            }
            value = value * 10 + (character - '0');
        }
        return value;
    };
    if (text.size() < 16 || (text[10] != 'T' && text[10] != ' ') || text[13] != ':') {
        return fail("date must carry a time of day");
    }
    const int year = read_number(0, 4, "date year is not a four digit number");
    const int month = read_number(5, 2, "date month is not a two digit number");
    const int day = read_number(8, 2, "date day is not a two digit number");
    const int hour = read_number(11, 2, "date hour is not a two digit number");
    const int minute = read_number(14, 2, "date minute is not a two digit number");
    if (year < 0 || month < 0 || day < 0 || hour < 0 || minute < 0) {
        return std::nullopt;
    }
    static constexpr int days_per_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month < 1 || month > 12) {
        return fail("date month is outside 1-12");
    }
    const int last_day = month == 2 && leap ? 29 : days_per_month[month];
    if (day < 1 || day > last_day) {
        return fail("date day is outside the month");
    }
    if (hour > 23 || minute > 59) {
        return fail("date time of day is outside the day");
    }
    int second = 0;
    std::size_t offset = 16;
    if (offset < text.size() && text[offset] == ':') {
        if (text.size() < offset + 3) {
            return fail("date seconds field is incomplete");
        }
        second = read_number(offset + 1, 2, "date second is not a two digit number");
        if (second < 0) {
            return std::nullopt;
        }
        if (second > 59) {
            return fail("date second is outside 0-59");
        }
        offset += 3;
    }
    std::uint64_t nanoseconds = 0;
    if (offset < text.size() && text[offset] == '.') {
        std::size_t digits = 0;
        ++offset;
        while (offset < text.size() && text[offset] >= '0' && text[offset] <= '9') {
            if (digits >= 9) {
                return fail("date fraction has more than nine digits");
            }
            nanoseconds = nanoseconds * 10 + static_cast<std::uint64_t>(text[offset] - '0');
            ++digits;
            ++offset;
        }
        if (digits == 0) {
            return fail("date fraction has no digits");
        }
        for (; digits < 9; ++digits) {
            nanoseconds *= 10;
        }
    }
    if (offset != text.size()) {
        return fail("date value has trailing characters");
    }

    // Howard Hinnant's days_from_civil, with 1970-01-01 as day zero.
    const std::int64_t shifted_year = year - (month <= 2 ? 1 : 0);
    const std::int64_t era = (shifted_year >= 0 ? shifted_year : shifted_year - 399) / 400;
    const std::int64_t year_of_era = shifted_year - era * 400;
    const std::int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const std::int64_t day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const std::int64_t unix_days = era * 146097 + day_of_era - 719468;

    constexpr std::int64_t access_epoch_days_from_unix = -25569;
    constexpr std::int64_t seconds_per_day = 86400;
    constexpr std::uint64_t nanoseconds_per_second = 1000000000ull;
    const std::int64_t access_days = unix_days - access_epoch_days_from_unix;
    if (access_days < -657434 || access_days > 2958465) {
        return fail("date is outside the Access date range");
    }
    const std::uint64_t day_nanoseconds =
        (static_cast<std::uint64_t>(hour) * 3600 + static_cast<std::uint64_t>(minute) * 60 +
         static_cast<std::uint64_t>(second)) *
            nanoseconds_per_second +
        nanoseconds;
    const double day_fraction = static_cast<double>(day_nanoseconds) /
                                (static_cast<double>(seconds_per_day) *
                                 static_cast<double>(nanoseconds_per_second));
    return access_days < 0 ? static_cast<double>(access_days) - day_fraction
                           : static_cast<double>(access_days) + day_fraction;
}

Jet4Value decode_jet4_value(const PageFile& file,
                            const std::string& table_name,
                            const EncodedTableRow& row,
                            const ColumnDefinition& column) {
    if (column.column_number >= row.columns.size()) {
        value_error(file, table_name, row, column, "Decoded row does not contain the column");
    }
    const auto& encoded = row.columns[column.column_number];
    const std::uint8_t value_type = column.calculated_result_type.value_or(column.type);
    if (column.type == boolean_type && !column.calculated) {
        return encoded.boolean_value;
    }
    if (!encoded.bytes) {
        return std::monostate{};
    }
    const auto& stored_bytes = *encoded.bytes;
    std::vector<std::uint8_t> calculated_value;
    const std::vector<std::uint8_t>* value_bytes = &stored_bytes;
    if (column.calculated && value_type != memo_type && value_type != ole_type &&
        stored_bytes.size() >= calculated_value_data_offset) {
        // Calculated columns store a wrapped result; expose it without evaluating the expression.
        calculated_value = unwrap_calculated_value(file, table_name, row, column, stored_bytes);
        value_bytes = &calculated_value;
        const bool variable_length_type =
            value_type == binary_type || value_type == text_type || value_type == ole_type ||
            value_type == memo_type || value_type == numeric_type;
        if (calculated_value.empty() && !variable_length_type) {
            if (value_type == boolean_type) {
                return false;
            }
            return std::monostate{};
        }
        if (calculated_value.empty() && value_type == numeric_type) {
            return std::monostate{};
        }
    }
    const auto& bytes = *value_bytes;

    switch (value_type) {
        case boolean_type:
            if (bytes.size() != 1) {
                value_error(file, table_name, row, column, "Calculated Boolean value has an invalid byte length");
            }
            return bytes.front() != 0;
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
            if (value_type == date_type) {
                if (!std::isfinite(value)) {
                    return UnsupportedValue{"date serial is not finite", bytes};
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
            if (column.calculated) {
                if (bytes.size() < 4) {
                    value_error(file, table_name, row, column,
                                "Calculated NUMERIC value has a truncated header");
                }
                const auto encoded_length = static_cast<std::int16_t>(read_u16(bytes, 0));
                const std::size_t total_length = encoded_length > 0
                                                     ? static_cast<std::size_t>(encoded_length)
                                                     : bytes.size() - 2;
                if (total_length < 2) {
                    value_error(file, table_name, row, column,
                                "Calculated NUMERIC value has an invalid length");
                }
                const std::size_t unrounded_length = total_length - 2;
                const std::size_t numeric_length = std::min((unrounded_length / 4) * 4, std::size_t{16});
                if (numeric_length > bytes.size() - 4) {
                    value_error(file, table_name, row, column,
                                "Calculated NUMERIC value data is truncated");
                }
                const unsigned int scale = bytes[2];
                if (scale > 28) {
                    value_error(file, table_name, row, column,
                                "Calculated NUMERIC scale is outside the supported range");
                }
                std::vector<std::uint8_t> numeric_bytes(bytes.begin() + 4,
                                                        bytes.begin() + static_cast<std::ptrdiff_t>(4 + numeric_length));
                std::size_t offset = 0;
                // CalcNumeric swaps one leading 4-byte block, then each 8-byte block.
                if (numeric_bytes.size() % 8 != 0) {
                    std::reverse(numeric_bytes.begin(), numeric_bytes.begin() + 4);
                    offset = 4;
                }
                for (; offset < numeric_bytes.size(); offset += 8) {
                    std::reverse(numeric_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                 numeric_bytes.begin() + static_cast<std::ptrdiff_t>(offset + 8));
                }
                std::vector<std::uint8_t> big_endian(16, 0);
                std::copy(numeric_bytes.begin(), numeric_bytes.end(),
                          big_endian.end() - static_cast<std::ptrdiff_t>(numeric_bytes.size()));
                return DecimalValue{decimal_text_from_digits(unsigned_128_decimal(big_endian),
                                                              bytes[3] != 0,
                                                              scale)};
            }
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
            auto long_bytes = read_long_value(file, table_name, row, column, bytes);
            if (column.calculated) {
                long_bytes = unwrap_calculated_value(file, table_name, row, column, long_bytes);
            }
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
            return UnsupportedValue{"unsupported MDB type " + std::to_string(value_type), bytes};
    }
}

std::optional<std::int32_t> decode_jet4_complex_value_foreign_key(const PageFile& file,
                                                                  const std::string& table_name,
                                                                  const EncodedTableRow& row,
                                                                  const ColumnDefinition& column) {
    if (column.column_number >= row.columns.size()) {
        value_error(file, table_name, row, column, "Decoded row does not contain the complex column");
    }
    if (column.type != complex_type) {
        throw std::invalid_argument("Complex foreign key decoding requires an ACE complex column");
    }
    const auto& encoded = row.columns[column.column_number];
    if (!encoded.bytes) {
        return std::nullopt;
    }
    if (encoded.bytes->size() != 4) {
        value_error(file, table_name, row, column, "Complex value foreign key has an invalid byte length");
    }

    const std::uint32_t bits = read_u32(*encoded.bytes, 0);
    if (bits <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return static_cast<std::int32_t>(bits);
    }
    return static_cast<std::int32_t>(-1 - static_cast<std::int64_t>(~bits));
}

std::int64_t jet4_money_scaled_value(std::string_view text) {
    if (text.empty()) {
        throw std::invalid_argument("MONEY value must be a decimal number");
    }

    std::size_t index = 0;
    bool negative = false;
    if (text[index] == '+' || text[index] == '-') {
        negative = text[index] == '-';
        if (++index == text.size()) {
            throw std::invalid_argument("MONEY value must be a decimal number");
        }
    }

    std::string digits;
    digits.reserve(text.size());
    std::size_t fraction_digit_count = 0;
    bool decimal_separator_seen = false;
    bool digit_seen = false;
    for (; index < text.size() && text[index] != 'e' && text[index] != 'E'; ++index) {
        const char character = text[index];
        if (character == '.') {
            if (decimal_separator_seen) {
                throw std::invalid_argument("MONEY value must be a decimal number");
            }
            decimal_separator_seen = true;
            continue;
        }
        if (character < '0' || character > '9') {
            throw std::invalid_argument("MONEY value must be a decimal number");
        }
        digit_seen = true;
        digits.push_back(character);
        if (decimal_separator_seen) {
            ++fraction_digit_count;
        }
    }
    if (!digit_seen) {
        throw std::invalid_argument("MONEY value must be a decimal number");
    }

    bool negative_exponent = false;
    std::uint64_t exponent_magnitude = 0;
    if (index < text.size()) {
        ++index;
        if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
            negative_exponent = text[index] == '-';
            ++index;
        }
        if (index == text.size()) {
            throw std::invalid_argument("MONEY exponent must contain decimal digits");
        }
        for (; index < text.size(); ++index) {
            const char character = text[index];
            if (character < '0' || character > '9') {
                throw std::invalid_argument("MONEY exponent must contain decimal digits");
            }
            const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
            if (exponent_magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                throw std::invalid_argument("MONEY exponent is outside the decimal scale range");
            }
            exponent_magnitude = exponent_magnitude * 10 + digit;
        }
    }

    constexpr std::uint64_t maximum_scale =
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    constexpr std::uint64_t minimum_scale_magnitude = maximum_scale + 1;
    const std::uint64_t fraction_digits = static_cast<std::uint64_t>(fraction_digit_count);
    std::int64_t scale = 0;
    if (negative_exponent) {
        if (fraction_digits > maximum_scale || exponent_magnitude > maximum_scale - fraction_digits) {
            throw std::invalid_argument("MONEY decimal scale is outside the supported range");
        }
        scale = static_cast<std::int64_t>(fraction_digits + exponent_magnitude);
    } else if (fraction_digits >= exponent_magnitude) {
        const std::uint64_t positive_scale = fraction_digits - exponent_magnitude;
        if (positive_scale > maximum_scale) {
            throw std::invalid_argument("MONEY decimal scale is outside the supported range");
        }
        scale = static_cast<std::int64_t>(positive_scale);
    } else {
        const std::uint64_t negative_scale = exponent_magnitude - fraction_digits;
        if (negative_scale > minimum_scale_magnitude) {
            throw std::invalid_argument("MONEY decimal scale is outside the supported range");
        }
        scale = -static_cast<std::int64_t>(negative_scale);
    }

    const std::int64_t shift = 4 - scale;
    const auto first_significant = digits.find_first_not_of('0');
    if (first_significant == std::string::npos) {
        return 0;
    }
    digits.erase(0, first_significant);
    if (shift < 0) {
        const std::uint64_t removed_digits = static_cast<std::uint64_t>(-(shift + 1)) + 1;
        if (removed_digits >= digits.size()) {
            throw std::invalid_argument("MONEY value has more than four non-zero decimal places");
        }
        const std::size_t retained_size = digits.size() - static_cast<std::size_t>(removed_digits);
        if (std::any_of(digits.begin() + static_cast<std::ptrdiff_t>(retained_size), digits.end(), [](char digit) {
                return digit != '0';
            })) {
            throw std::invalid_argument("MONEY value has more than four non-zero decimal places");
        }
        digits.resize(retained_size);
    } else if (shift > 0) {
        if (shift > 19 || digits.size() + static_cast<std::size_t>(shift) > 19) {
            throw std::invalid_argument("MONEY value exceeds the Jet4 currency range");
        }
        digits.append(static_cast<std::size_t>(shift), '0');
    }

    constexpr std::uint64_t min_magnitude = std::uint64_t{1} << 63;
    constexpr std::uint64_t max_positive = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const std::uint64_t max_magnitude = negative ? min_magnitude : max_positive;
    const std::string max_magnitude_text = std::to_string(max_magnitude);
    if (digits.size() > max_magnitude_text.size() ||
        (digits.size() == max_magnitude_text.size() && digits > max_magnitude_text)) {
        throw std::invalid_argument("MONEY value exceeds the Jet4 currency range");
    }

    std::uint64_t magnitude = 0;
    for (const char digit : digits) {
        magnitude = magnitude * 10 + static_cast<std::uint64_t>(digit - '0');
    }

    if (!negative) {
        return static_cast<std::int64_t>(magnitude);
    }
    if (magnitude == min_magnitude) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
}

std::array<std::uint8_t, 16> jet4_guid_bytes(std::string_view text) {
    if (text.size() == 38 && text.front() == '{' && text.back() == '}') {
        text.remove_prefix(1);
        text.remove_suffix(1);
    }
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') {
        throw std::invalid_argument("GUID value must use canonical UUID text with optional braces");
    }

    auto hex_digit = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };

    std::array<std::uint8_t, 16> canonical_bytes{};
    std::size_t byte_index = 0;
    for (std::size_t offset = 0; offset < text.size();) {
        if (text[offset] == '-') {
            ++offset;
            continue;
        }
        if (byte_index >= canonical_bytes.size() || offset + 1 >= text.size()) {
            throw std::invalid_argument("GUID value contains invalid hexadecimal data");
        }
        const int high = hex_digit(text[offset]);
        const int low = hex_digit(text[offset + 1]);
        if (high < 0 || low < 0) {
            throw std::invalid_argument("GUID value contains invalid hexadecimal data");
        }
        canonical_bytes[byte_index++] = static_cast<std::uint8_t>((high << 4) | low);
        offset += 2;
    }
    if (byte_index != canonical_bytes.size()) {
        throw std::invalid_argument("GUID value must contain exactly 16 bytes");
    }

    return {canonical_bytes[3], canonical_bytes[2], canonical_bytes[1], canonical_bytes[0],
            canonical_bytes[5], canonical_bytes[4], canonical_bytes[7], canonical_bytes[6],
            canonical_bytes[8], canonical_bytes[9], canonical_bytes[10], canonical_bytes[11],
            canonical_bytes[12], canonical_bytes[13], canonical_bytes[14], canonical_bytes[15]};
}

}
