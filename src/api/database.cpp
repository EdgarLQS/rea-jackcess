#include "database_internal.hpp"

#include "catalog/complex_column.hpp"
#include "catalog/property_map.hpp"
#include "catalog/row_reader.hpp"
#include "catalog/system_catalog.hpp"
#include "index/index_cursor.hpp"
#include "page/write_journal.hpp"
#include "value/attachment_value.hpp"
#include "value/value_decode.hpp"
#include "value/text_codec.hpp"
#include "value/unicode_case.hpp"
#include "write/jet4_database_creator.hpp"
#include "write/jet4_index_entry.hpp"
#include "write/jet4_relationship_creator.hpp"
#include "write/jet4_row_writer.hpp"
#include "write/jet4_table_creator.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

namespace rea_jackcess {
namespace {

DataType public_data_type(std::uint8_t type_code) {
    switch (type_code) {
        case 0x01:
            return DataType::boolean;
        case 0x02:
            return DataType::byte;
        case 0x03:
            return DataType::int16;
        case 0x04:
            return DataType::int32;
        case 0x05:
            return DataType::money;
        case 0x06:
            return DataType::float32;
        case 0x07:
            return DataType::float64;
        case 0x08:
            return DataType::short_date_time;
        case 0x09:
            return DataType::binary;
        case 0x0a:
            return DataType::text;
        case 0x0b:
            return DataType::ole;
        case 0x0c:
            return DataType::memo;
        case 0x0f:
            return DataType::guid;
        case 0x10:
            return DataType::numeric;
        case 0x11:
            return DataType::big_binary;
        case 0x12:
            return DataType::complex;
        case 0x13:
            return DataType::int64;
        case 0x14:
            return DataType::extended_date_time;
        default:
            return DataType::unknown;
    }
}

std::vector<IndexInfo> public_indexes(const detail::TableDefinition& definition) {
    constexpr std::uint8_t unique_index_flag = 0x01;
    constexpr std::uint8_t ignore_nulls_index_flag = 0x02;
    constexpr std::uint8_t required_index_flag = 0x08;
    constexpr std::uint8_t primary_key_index_type = 0x01;
    constexpr std::uint8_t foreign_key_index_type = 0x02;
    constexpr std::uint8_t cascade_flag = 0x01;
    constexpr std::uint8_t cascade_null_flag = 0x02;

    std::vector<bool> physical_is_primary(definition.physical_indexes.size(), false);
    for (const auto& index : definition.indexes) {
        if (index.type == primary_key_index_type) {
            physical_is_primary[index.index_data_number] = true;
        }
    }

    std::vector<IndexInfo> indexes;
    indexes.reserve(definition.indexes.size());
    for (const auto& index : definition.indexes) {
        const auto& physical = definition.physical_indexes[index.index_data_number];
        IndexInfo result{
            index.name,
            index.index_number,
            index.index_data_number,
            {},
            physical.root_page_number,
            physical.unique_entry_count,
            physical.flags,
            physical_is_primary[index.index_data_number] || (physical.flags & unique_index_flag) != 0,
            (physical.flags & ignore_nulls_index_flag) != 0,
            (physical.flags & required_index_flag) != 0,
            index.type == primary_key_index_type,
            index.type == foreign_key_index_type,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            (index.cascade_updates_flags & cascade_flag) != 0,
            (index.cascade_deletes_flags & cascade_flag) != 0,
            (index.cascade_deletes_flags & cascade_null_flag) != 0,
        };
        result.columns.reserve(physical.columns.size());
        for (const auto& indexed_column : physical.columns) {
            const auto column = std::find_if(definition.columns.begin(), definition.columns.end(), [&](const auto& candidate) {
                return candidate.column_number == indexed_column.column_number;
            });
            if (column == definition.columns.end()) {
                throw std::logic_error("Index references a column absent from its table definition");
            }
            result.columns.push_back({column->name, indexed_column.column_number, indexed_column.ascending});
        }
        if (result.foreign_key && index.referenced_index_number >= 0) {
            result.referenced_index_type = index.referenced_index_type;
            result.referenced_index_number = static_cast<std::uint32_t>(index.referenced_index_number);
            result.referenced_table_page_number = index.referenced_table_page_number;
        }
        indexes.push_back(std::move(result));
    }
    return indexes;
}

const detail::DatabaseState& checked_state(const std::shared_ptr<detail::DatabaseState>& state) {
    if (!state) {
        throw std::logic_error("Database has no state; it may have been moved from");
    }
    return *state;
}

const detail::TableState& checked_state(const std::shared_ptr<detail::TableState>& state) {
    if (!state) {
        throw std::logic_error("Table has no state; it may have been moved from");
    }
    return *state;
}

}

namespace detail {
namespace {

ComplexColumnType public_complex_column_type(ComplexColumnKind type) {
    switch (type) {
        case ComplexColumnKind::multi_value:
            return ComplexColumnType::multi_value;
        case ComplexColumnKind::attachment:
            return ComplexColumnType::attachment;
        case ComplexColumnKind::version_history:
            return ComplexColumnType::version_history;
        case ComplexColumnKind::unsupported:
            return ComplexColumnType::unsupported;
    }
    return ComplexColumnType::unsupported;
}

}

DatabaseState::DatabaseState(PageFile source_file,
                             TableSchemaCatalog source_schema,
                             bool source_writable,
                             std::optional<PageFileWriteLock> source_write_lock)
    : file(std::move(source_file)),
      schema(std::move(source_schema)),
      writable(source_writable),
      write_lock(std::move(source_write_lock)) {
    columns.reserve(schema.tables.size());
    column_validators.reserve(schema.tables.size());
    indexes.reserve(schema.tables.size());
    for (const auto& table : schema.tables) {
        std::vector<ColumnInfo> table_columns;
        table_columns.reserve(table.definition.columns.size());
        for (const auto& column : table.definition.columns) {
            table_columns.push_back({column.name,
                                     public_data_type(column.calculated_result_type.value_or(column.type)),
                                     column.type,
                                     column.length,
                                     column.precision,
                                     column.scale,
                                     (column.flags & 0x01) != 0,
                                     column.calculated,
                                     column.nullable,
                                     column.complex_type_id,
                                     column.complex_value_foreign_key});
        }
        columns.push_back(std::move(table_columns));
        column_validators.emplace_back(table.definition.columns.size());
        indexes.push_back(public_indexes(table.definition));
    }
    diagnostics.reserve(schema.errors.size());
    for (const auto& error : schema.errors) {
        std::optional<std::string> table_name;
        if (!error.table_name.empty()) {
            table_name = error.table_name;
        }
        diagnostics.push_back({std::move(table_name), error.message, error.page_number});
    }

    const auto complex_column_catalog = read_complex_column_catalog(file, schema);
    for (const auto& error : complex_column_catalog.errors) {
        diagnostics.push_back({error.table_name, error.message, error.page_number});
    }
    for (const auto& association : complex_column_catalog.associations) {
        std::optional<ComplexColumnType> type;
        if (association.type) {
            type = public_complex_column_type(*association.type);
        }
        columns.at(association.conceptual_table_index)
            .at(association.complex_column_index)
            .complex_column_reference = ComplexColumnReference{
            association.conceptual_table_page_number,
            association.complex_type_table_page_number,
            association.complex_type_table_name,
            type,
            association.flat_table_page_number,
            association.flat_table_name,
            association.primary_key_column_name,
            association.foreign_key_column_name,
        };
    }
}

}  // namespace detail

namespace {

const detail::ColumnDefinition& resolve_index_column(const detail::TableState& state,
                                                     const IndexColumnInfo& indexed) {
    const auto& columns = state.database->schema.tables.at(state.schema_index).definition.columns;
    const auto column = std::find_if(columns.begin(), columns.end(), [&](const auto& candidate) {
        return candidate.column_number == indexed.column_number;
    });
    if (column == columns.end()) {
        throw DatabaseError(DatabaseErrorCode::corrupted_file, "Index references an unknown column",
                            state.database->file.path());
    }
    return *column;
}

struct IndexPrefixPart {
    const detail::ColumnDefinition* column;
    bool descending;
    std::vector<std::uint8_t> key;
};

std::uint8_t ascii_lower(std::uint8_t character) {
    return character >= 'A' && character <= 'Z'
               ? static_cast<std::uint8_t>(character + ('a' - 'A'))
               : character;
}

bool column_names_equal(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char lhs, char rhs) {
               return ascii_lower(static_cast<std::uint8_t>(lhs)) ==
                      ascii_lower(static_cast<std::uint8_t>(rhs));
           });
}

bool values_equal(const Value& left, const Value& right) {
    if (left.index() != right.index()) {
        return false;
    }
    return std::visit([&](const auto& lhs) {
        using Type = std::decay_t<decltype(lhs)>;
        if constexpr (std::is_same_v<Type, std::monostate>) {
            return true;
        } else if constexpr (std::is_same_v<Type, DecimalValue>) {
            return lhs.text == std::get<Type>(right).text;
        } else if constexpr (std::is_same_v<Type, DateTimeValue>) {
            return lhs.iso8601 == std::get<Type>(right).iso8601;
        } else if constexpr (std::is_same_v<Type, DateOutOfRangeValue>) {
            return lhs.serial == std::get<Type>(right).serial;
        } else if constexpr (std::is_same_v<Type, GuidValue>) {
            return lhs.text == std::get<Type>(right).text;
        } else if constexpr (std::is_same_v<Type, MemoValue>) {
            const auto& rhs = std::get<Type>(right);
            return lhs.text == rhs.text && lhs.raw_bytes == rhs.raw_bytes;
        } else if constexpr (std::is_same_v<Type, BlobValue>) {
            return lhs.bytes == std::get<Type>(right).bytes;
        } else if constexpr (std::is_same_v<Type, UnsupportedValue>) {
            return lhs.description == std::get<Type>(right).description;
        } else if constexpr (std::is_same_v<Type, float> || std::is_same_v<Type, double>) {
            const Type rhs = std::get<Type>(right);
            return (std::isnan(lhs) && std::isnan(rhs)) ||
                   (lhs == rhs && (lhs != 0 || std::signbit(lhs) == std::signbit(rhs)));
        } else {
            return lhs == std::get<Type>(right);
        }
    }, left);
}

bool is_textual(DataType type) {
    return type == DataType::text || type == DataType::memo;
}

bool ascii_case_insensitive_equal(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char lhs, char rhs) {
               return ascii_lower(static_cast<std::uint8_t>(lhs)) ==
                      ascii_lower(static_cast<std::uint8_t>(rhs));
           });
}

template <typename Float>
std::string floating_text(Float value) {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return std::signbit(value) ? "-Infinity" : "Infinity";
    }
    std::array<char, 128> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                      std::chars_format::general);
    if (result.ec != std::errc{}) {
        throw std::invalid_argument("Could not format floating point value");
    }
    std::string text(buffer.data(), result.ptr);
    const auto exponent = text.find_first_of("eE");
    const auto decimal = text.find('.');
    if (decimal == std::string::npos) {
        text.insert(exponent == std::string::npos ? text.size() : exponent, ".0");
    }
    return text;
}

bool is_java_double_literal(std::string_view text) {
    while (!text.empty() && static_cast<unsigned char>(text.front()) <= 0x20) {
        text.remove_prefix(1);
    }
    while (!text.empty() && static_cast<unsigned char>(text.back()) <= 0x20) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return false;
    }

    std::size_t position = 0;
    if (text[position] == '+' || text[position] == '-') {
        ++position;
    }
    const std::string_view signed_value = text.substr(position);
    if (signed_value == "NaN" || signed_value == "Infinity") {
        return true;
    }

    const bool hexadecimal = position + 2 <= text.size() && text[position] == '0' &&
                             (text[position + 1] == 'x' || text[position + 1] == 'X');
    if (hexadecimal) {
        position += 2;
    }

    const auto consume_digits = [&](bool hex) {
        const std::size_t start = position;
        while (position < text.size()) {
            const char character = text[position];
            const bool digit = character >= '0' && character <= '9';
            const bool hex_digit = (character >= 'a' && character <= 'f') ||
                                   (character >= 'A' && character <= 'F');
            if (!digit && !(hex && hex_digit)) {
                break;
            }
            ++position;
        }
        return position != start;
    };

    bool has_digits = consume_digits(hexadecimal);
    if (position < text.size() && text[position] == '.') {
        ++position;
        has_digits = consume_digits(hexadecimal) || has_digits;
    }
    if (!has_digits) {
        return false;
    }

    const char exponent_marker = hexadecimal ? 'p' : 'e';
    const char alternate_exponent_marker = hexadecimal ? 'P' : 'E';
    const bool has_exponent = position < text.size() &&
                              (text[position] == exponent_marker ||
                               text[position] == alternate_exponent_marker);
    if (hexadecimal && !has_exponent) {
        return false;
    }
    if (has_exponent) {
        ++position;
        if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
            ++position;
        }
        if (!consume_digits(false)) {
            return false;
        }
    }

    if (position < text.size() &&
        (text[position] == 'f' || text[position] == 'F' || text[position] == 'd' ||
         text[position] == 'D')) {
        ++position;
    }
    return position == text.size();
}

double parse_double_text(const std::string& text) {
    if (!is_java_double_literal(text)) {
        throw std::invalid_argument("Value cannot be converted to a number");
    }
    std::size_t start = 0;
    while (start < text.size() && static_cast<unsigned char>(text[start]) <= 0x20) {
        ++start;
    }
    std::size_t end_position = text.size();
    while (end_position > start && static_cast<unsigned char>(text[end_position - 1]) <= 0x20) {
        --end_position;
    }
    std::string normalized = text.substr(start, end_position - start);
    if (!normalized.empty() && (normalized.back() == 'f' || normalized.back() == 'F' ||
                                normalized.back() == 'd' || normalized.back() == 'D')) {
        normalized.pop_back();
    }
    char* end = nullptr;
    const double value = std::strtod(normalized.c_str(), &end);
    if (end == normalized.c_str() || *end != '\0') {
        throw std::invalid_argument("Value cannot be converted to a number");
    }
    return value;
}

std::optional<std::string> value_as_text(const Value& value) {
    return std::visit([](const auto& item) -> std::optional<std::string> {
        using Type = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Type, std::monostate>) {
            return std::nullopt;
        } else if constexpr (std::is_same_v<Type, bool>) {
            return item ? "true" : "false";
        } else if constexpr (std::is_same_v<Type, std::int8_t> ||
                             std::is_same_v<Type, std::int16_t> ||
                             std::is_same_v<Type, std::int32_t> ||
                             std::is_same_v<Type, std::int64_t>) {
            return std::to_string(item);
        } else if constexpr (std::is_same_v<Type, float> || std::is_same_v<Type, double>) {
            return floating_text(item);
        } else if constexpr (std::is_same_v<Type, std::string>) {
            return item;
        } else if constexpr (std::is_same_v<Type, DecimalValue>) {
            return item.text;
        } else if constexpr (std::is_same_v<Type, DateTimeValue>) {
            return item.iso8601;
        } else if constexpr (std::is_same_v<Type, DateOutOfRangeValue>) {
            return floating_text(item.serial);
        } else if constexpr (std::is_same_v<Type, GuidValue>) {
            return item.text;
        } else if constexpr (std::is_same_v<Type, MemoValue>) {
            return item.text;
        } else if constexpr (std::is_same_v<Type, UnsupportedValue>) {
            return item.description;
        } else {
            return std::nullopt;
        }
    }, value);
}

std::optional<double> value_as_number(const Value& value) {
    return std::visit([](const auto& item) -> std::optional<double> {
        using Type = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Type, bool>) {
            return item ? -1.0 : 0.0;
        } else if constexpr (std::is_same_v<Type, std::int8_t> ||
                             std::is_same_v<Type, std::int16_t> ||
                             std::is_same_v<Type, std::int32_t> ||
                             std::is_same_v<Type, std::int64_t> ||
                             std::is_same_v<Type, float> || std::is_same_v<Type, double>) {
            return static_cast<double>(item);
        } else if constexpr (std::is_same_v<Type, DecimalValue>) {
            return parse_double_text(item.text);
        } else if constexpr (std::is_same_v<Type, std::string>) {
            return parse_double_text(item);
        } else if constexpr (std::is_same_v<Type, DateOutOfRangeValue>) {
            return item.serial;
        } else {
            return std::nullopt;
        }
    }, value);
}

std::int64_t java_long_value(double value) {
    if (std::isnan(value)) {
        return 0;
    }
    if (value >= 9223372036854775808.0) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (value <= -9223372036854775808.0) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
}

std::optional<std::int64_t> value_as_integer(const Value& value) {
    return std::visit([](const auto& item) -> std::optional<std::int64_t> {
        using Type = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Type, bool>) {
            return item ? -1 : 0;
        } else if constexpr (std::is_same_v<Type, std::int8_t> ||
                             std::is_same_v<Type, std::int16_t> ||
                             std::is_same_v<Type, std::int32_t> ||
                             std::is_same_v<Type, std::int64_t>) {
            return static_cast<std::int64_t>(item);
        } else if constexpr (std::is_same_v<Type, float> || std::is_same_v<Type, double>) {
            return java_long_value(static_cast<double>(item));
        } else if constexpr (std::is_same_v<Type, DecimalValue>) {
            return java_long_value(parse_double_text(item.text));
        } else if constexpr (std::is_same_v<Type, std::string>) {
            return java_long_value(parse_double_text(item));
        } else if constexpr (std::is_same_v<Type, DateOutOfRangeValue>) {
            return java_long_value(item.serial);
        } else {
            return std::nullopt;
        }
    }, value);
}

std::int8_t narrow_int8(std::int64_t value) {
    const auto bits = static_cast<std::uint8_t>(value);
    return bits <= 0x7f ? static_cast<std::int8_t>(bits)
                        : static_cast<std::int8_t>(static_cast<std::int16_t>(bits) - 0x100);
}

std::int16_t narrow_int16(std::int64_t value) {
    const auto bits = static_cast<std::uint16_t>(value);
    return bits <= 0x7fff ? static_cast<std::int16_t>(bits)
                          : static_cast<std::int16_t>(static_cast<std::int32_t>(bits) - 0x10000);
}

std::int32_t narrow_int32(std::int64_t value) {
    const auto bits = static_cast<std::uint32_t>(value);
    return bits <= 0x7fffffffU ? static_cast<std::int32_t>(bits)
                               : static_cast<std::int32_t>(static_cast<std::int64_t>(bits) -
                                                           0x100000000LL);
}

std::optional<Value> java_serialized_string(std::string_view value) {
    // ColumnImpl.toByteArray serializes non-byte BINARY values with ObjectOutputStream.
    std::u16string utf16;
    try {
        utf16 = detail::utf8_to_utf16(value, std::numeric_limits<std::size_t>::max(),
                                      "Serialized string is too large");
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }

    if (utf16.size() > std::numeric_limits<std::size_t>::max() / 3) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> modified_utf8;
    modified_utf8.reserve(utf16.size() * 3);
    for (const char16_t unit : utf16) {
        const auto code_unit = static_cast<std::uint16_t>(unit);
        if (code_unit != 0 && code_unit <= 0x7f) {
            modified_utf8.push_back(static_cast<std::uint8_t>(code_unit));
        } else if (code_unit <= 0x07ff) {
            modified_utf8.push_back(static_cast<std::uint8_t>(0xc0 | (code_unit >> 6)));
            modified_utf8.push_back(static_cast<std::uint8_t>(0x80 | (code_unit & 0x3f)));
        } else {
            modified_utf8.push_back(static_cast<std::uint8_t>(0xe0 | (code_unit >> 12)));
            modified_utf8.push_back(static_cast<std::uint8_t>(0x80 | ((code_unit >> 6) & 0x3f)));
            modified_utf8.push_back(static_cast<std::uint8_t>(0x80 | (code_unit & 0x3f)));
        }
    }

    std::vector<std::uint8_t> serialized{0xac, 0xed, 0x00, 0x05};
    if (modified_utf8.size() <= std::numeric_limits<std::uint16_t>::max()) {
        serialized.push_back(0x74); // TC_STRING
        serialized.push_back(static_cast<std::uint8_t>(modified_utf8.size() >> 8));
        serialized.push_back(static_cast<std::uint8_t>(modified_utf8.size() & 0xff));
    } else {
        serialized.push_back(0x7c); // TC_LONGSTRING
        const auto length = static_cast<std::uint64_t>(modified_utf8.size());
        for (int shift = 56; shift >= 0; shift -= 8) {
            serialized.push_back(static_cast<std::uint8_t>((length >> shift) & 0xff));
        }
    }
    serialized.insert(serialized.end(), modified_utf8.begin(), modified_utf8.end());
    return BlobValue{std::move(serialized)};
}

std::optional<Value> value_as_internal(DataType type, const Value& value) {
    if (type == DataType::boolean) {
        if (const auto* boolean = std::get_if<bool>(&value)) {
            return *boolean;
        }
        if (const auto* number = std::get_if<std::int8_t>(&value)) {
            return *number != 0;
        }
        if (const auto* number = std::get_if<std::int16_t>(&value)) {
            return *number != 0;
        }
        if (const auto* number = std::get_if<std::int32_t>(&value)) {
            return *number != 0;
        }
        if (const auto* number = std::get_if<std::int64_t>(&value)) {
            return *number != 0;
        }
        if (const auto* number = std::get_if<float>(&value)) {
            return *number != 0.0f;
        }
        if (const auto* number = std::get_if<double>(&value)) {
            return *number != 0.0;
        }
        if (const auto* decimal = std::get_if<DecimalValue>(&value)) {
            return std::any_of(decimal->text.begin(), decimal->text.end(), [](char character) {
                return character >= '1' && character <= '9';
            });
        }
        if (const auto text = value_as_text(value)) {
            return ascii_case_insensitive_equal(*text, "true");
        }
        return std::nullopt;
    }
    if (type == DataType::byte || type == DataType::int16 || type == DataType::int32 ||
        type == DataType::int64) {
        const auto number = value_as_integer(value);
        if (!number) {
            return std::nullopt;
        }
        if (type == DataType::byte) {
            return narrow_int8(*number);
        }
        if (type == DataType::int16) {
            return narrow_int16(*number);
        }
        if (type == DataType::int32) {
            return narrow_int32(*number);
        }
        return *number;
    }
    if (type == DataType::float32 || type == DataType::float64) {
        const auto number = value_as_number(value);
        if (!number) {
            return std::nullopt;
        }
        if (type == DataType::float32) {
            return static_cast<float>(*number);
        }
        return *number;
    }
    if (type == DataType::short_date_time) {
        std::optional<double> serial;
        if (const auto* date = std::get_if<DateTimeValue>(&value)) {
            serial = detail::jet4_date_serial(date->iso8601);
        } else if (const auto* text = std::get_if<std::string>(&value)) {
            serial = detail::jet4_date_serial(*text);
        }
        if (serial) {
            return Value{*serial};
        }
        return std::nullopt;
    }
    if (type == DataType::text || type == DataType::memo || type == DataType::guid) {
        const auto text = value_as_text(value);
        if (!text) {
            return std::nullopt;
        }
        return *text;
    }
    if (type == DataType::binary) {
        if (const auto* blob = std::get_if<BlobValue>(&value)) {
            return *blob;
        }
        if (const auto* text = std::get_if<std::string>(&value)) {
            return java_serialized_string(*text);
        }
    }
    return std::nullopt;
}

struct DecimalKey {
    std::string unscaled;
    std::int64_t scale;

    bool operator==(const DecimalKey& other) const {
        return unscaled == other.unscaled && scale == other.scale;
    }
};

DecimalKey parse_decimal_key(const std::string& text) {
    std::size_t position = 0;
    bool negative = false;
    if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
        negative = text[position] == '-';
        ++position;
    }

    std::string digits;
    std::size_t fractional_digits = 0;
    bool saw_digit = false;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
        digits.push_back(text[position++]);
        saw_digit = true;
    }
    if (position < text.size() && text[position] == '.') {
        ++position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            digits.push_back(text[position++]);
            ++fractional_digits;
            saw_digit = true;
        }
    }
    if (!saw_digit) {
        throw std::invalid_argument("Value cannot be converted to a decimal number");
    }

    std::int64_t exponent = 0;
    if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        bool exponent_negative = false;
        if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
            exponent_negative = text[position] == '-';
            ++position;
        }
        const auto exponent_start = position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            ++position;
        }
        if (position == exponent_start) {
            throw std::invalid_argument("Value cannot be converted to a decimal number");
        }
        const auto result = std::from_chars(text.data() + exponent_start, text.data() + position,
                                            exponent);
        if (result.ec != std::errc{}) {
            throw std::invalid_argument("Decimal exponent is out of range");
        }
        if (exponent_negative) {
            exponent = -exponent;
        }
    }
    if (position != text.size()) {
        throw std::invalid_argument("Value cannot be converted to a decimal number");
    }

    auto first_nonzero = digits.find_first_not_of('0');
    if (first_nonzero == std::string::npos) {
        digits = "0";
        negative = false;
    } else if (first_nonzero != 0) {
        digits.erase(0, first_nonzero);
    }
    if (negative) {
        digits.insert(digits.begin(), '-');
    }
    const auto scale = static_cast<std::int64_t>(fractional_digits) - exponent;
    return {std::move(digits), scale};
}

std::optional<std::string> value_as_decimal_text(const Value& value) {
    return std::visit([](const auto& item) -> std::optional<std::string> {
        using Type = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Type, std::monostate>) {
            return std::nullopt;
        } else if constexpr (std::is_same_v<Type, DecimalValue>) {
            return item.text;
        } else if constexpr (std::is_same_v<Type, bool>) {
            return item ? "-1" : "0";
        } else if constexpr (std::is_same_v<Type, std::int8_t> ||
                             std::is_same_v<Type, std::int16_t> ||
                             std::is_same_v<Type, std::int32_t> ||
                             std::is_same_v<Type, std::int64_t>) {
            return floating_text(static_cast<double>(item));
        } else if constexpr (std::is_same_v<Type, float> || std::is_same_v<Type, double>) {
            return floating_text(static_cast<double>(item));
        } else if constexpr (std::is_same_v<Type, std::string>) {
            return item;
        } else if constexpr (std::is_same_v<Type, DateOutOfRangeValue>) {
            return floating_text(item.serial);
        } else {
            return std::nullopt;
        }
    }, value);
}

bool simple_values_match(DataType type, const Value& expected, const Value& actual) {
    if (values_equal(expected, actual)) {
        return true;
    }
    if (std::holds_alternative<std::monostate>(expected) ||
        std::holds_alternative<std::monostate>(actual)) {
        return false;
    }
    if (type == DataType::money || type == DataType::numeric) {
        const auto expected_text = value_as_decimal_text(expected);
        const auto actual_text = value_as_decimal_text(actual);
        return expected_text && actual_text &&
               parse_decimal_key(*expected_text) == parse_decimal_key(*actual_text);
    }
    const auto expected_internal = value_as_internal(type, expected);
    const auto actual_internal = value_as_internal(type, actual);
    return expected_internal && actual_internal && values_equal(*expected_internal, *actual_internal);
}

bool default_values_match(DataType type, const Value& expected, const Value& actual,
                          bool unique_index) {
    if (unique_index && is_textual(type) &&
        !std::holds_alternative<std::monostate>(expected) &&
        !std::holds_alternative<std::monostate>(actual)) {
        const auto expected_text = value_as_text(expected);
        const auto actual_text = value_as_text(actual);
        return expected_text && actual_text &&
               detail::java_string_equals_ignore_case_utf8(*expected_text, *actual_text);
    }
    return simple_values_match(type, expected, actual);
}

}

struct RowCursor::Impl {
    Impl(std::shared_ptr<detail::TableState> table_state,
         std::optional<std::uint32_t> index_root_page_number = std::nullopt,
         bool unique_index = false,
         std::optional<std::vector<std::uint8_t>> start_key = std::nullopt,
         std::optional<std::vector<std::uint8_t>> stop_key = std::nullopt,
         bool reverse = false,
         std::vector<std::uint16_t> projection_columns = {},
         RowFilter row_filter = {})
        : table(std::move(table_state)),
          index_root(index_root_page_number),
          start_key(std::move(start_key)),
          stop_key(std::move(stop_key)),
          unique_index(unique_index),
          reverse(reverse),
          projection(std::move(projection_columns)),
          filter(std::move(row_filter)),
          reader(table->database->file, table_name(), table_definition()) {
        std::lock_guard<std::recursive_mutex> lock(table->database->write_mutex);
        inherited_error_handler = table->error_handler
                                      ? table->error_handler
                                      : table->database->error_handler;
        error_handler = inherited_error_handler;
        if (!index_root && (this->start_key || this->stop_key)) {
            throw std::invalid_argument("Index bounds require an index cursor");
        }
        if (index_root) {
            index_reader = make_index_cursor();
        }
    }

    const std::string& table_name() const {
        return table->database->schema.tables[table->schema_index].name;
    }

    const detail::TableDefinition& table_definition() const {
        return table->database->schema.tables[table->schema_index].definition;
    }

    std::unique_ptr<detail::IndexEntryCursor> make_index_cursor() const {
        const auto& schema_table = table->database->schema.tables[table->schema_index];
        return std::make_unique<detail::IndexEntryCursor>(table->database->file, schema_table.name,
                                                          schema_table.definition_page_number,
                                                          *index_root, start_key, stop_key, reverse);
    }

    // Some encodings, such as an empty BINARY value, can be a prefix of another component, so the
    // leading components are compared after the bounded seek.
    bool matches_prefix(const detail::EncodedTableRow& candidate) const {
        return std::all_of(prefix.begin(), prefix.end(), [&](const IndexPrefixPart& part) {
            return detail::encode_jet4_index_key(*part.column,
                candidate.columns.at(part.column->column_number), part.descending) == part.key;
        });
    }

    std::vector<Value> decode(const detail::EncodedTableRow& candidate) const {
        const auto& definition = table_definition();
        std::vector<Value> values;
        if (projection.empty()) {
            values.reserve(definition.columns.size());
            for (const auto& column : definition.columns) {
                values.push_back(decode_column(candidate, column));
            }
            return values;
        }
        values.reserve(projection.size());
        for (const auto column_number : projection) {
            values.push_back(decode_column(candidate, definition.columns.at(column_number)));
        }
        return values;
    }

    Value decode_column(const detail::EncodedTableRow& candidate,
                       const detail::ColumnDefinition& column) const {
        try {
            return detail::decode_value(table->database->file, table_name(), candidate, column);
        } catch (const std::exception& error) {
            if (!error_handler) {
                throw;
            }
            std::optional<std::vector<std::uint8_t>> column_data;
            if (column.column_number < candidate.columns.size()) {
                column_data = candidate.columns[column.column_number].bytes;
            }
            const auto& public_column = table->database->columns.at(table->schema_index)
                                            .at(column.column_number);
            return error_handler(table_name(), public_column, column_data,
                                 {candidate.page_number, candidate.row_number}, error);
        }
    }

    using ResolvedPattern = std::vector<std::pair<std::uint16_t, const Value*>>;

    ResolvedPattern resolve_pattern(const RowPattern& pattern) const {
        const auto& columns = table_definition().columns;
        ResolvedPattern resolved;
        resolved.reserve(pattern.size());
        for (const auto& item : pattern) {
            const auto column = std::find_if(columns.begin(), columns.end(), [&](const auto& candidate) {
                return column_names_equal(candidate.name, item.first);
            });
            if (column == columns.end()) {
                throw std::invalid_argument("Table has no column named '" + item.first + "'");
            }
            const auto column_number = column->column_number;
            if (std::any_of(resolved.begin(), resolved.end(), [&](const auto& previous) {
                    return previous.first == column_number;
                })) {
                throw std::invalid_argument("Row pattern contains duplicate column names");
            }
            resolved.emplace_back(column_number, &item.second);
        }
        return resolved;
    }

    bool matches_pattern(const detail::EncodedTableRow& candidate,
                         const ResolvedPattern& pattern) const {
        const auto& columns = table_definition().columns;
        for (const auto& item : pattern) {
            const auto& column = columns.at(item.first);
            const Value actual = decode_column(candidate, column);
            const bool matched = column_matcher
                                     ? column_matcher(table_name(), column.name, *item.second, actual)
                                     : default_values_match(public_data_type(column.type),
                                                            *item.second, actual, unique_index);
            if (!matched) {
                return false;
            }
        }
        return true;
    }

    bool accepts(const detail::EncodedTableRow& candidate) const {
        if (!matches_prefix(candidate)) {
            return false;
        }
        if (!filter) {
            return true;
        }
        const Row row = RowCursor::make_row({candidate.page_number, candidate.row_number},
                                            decode(candidate));
        return filter(row);
    }

    // Pulls the next candidate from the streaming reader. The prefix check is applied here so a
    // bounded index cursor does not stop at a key that only shares the prefix bytes.
    std::optional<detail::EncodedTableRow> advance() {
        if (!index_reader) {
            return reader.next();
        }
        while (const auto location = index_reader->next()) {
            auto candidate = reader.read_at(location->page_number, location->row_number);
            if (matches_prefix(candidate)) {
                return candidate;
            }
        }
        return std::nullopt;
    }

    // Collects the rows the cursor would visit, in scan order, so navigation can move backwards
    // and jump to a RowId. The stream already consumed by next() is scanned again; a cursor is a
    // snapshot of the file, so the two passes agree.
    void materialize() {
        if (materialized) {
            return;
        }
        materialized = true;
        detail::TableRowReader scan(table->database->file, table_name(), table_definition());
        if (index_root) {
            auto index_scan = make_index_cursor();
            while (const auto location = index_scan->next()) {
                auto candidate = scan.read_at(location->page_number, location->row_number);
                if (accepts(candidate)) {
                    sequence.push_back({candidate.page_number, candidate.row_number});
                }
            }
        } else {
            while (const auto candidate = scan.next()) {
                if (accepts(*candidate)) {
                    sequence.push_back({candidate->page_number, candidate->row_number});
                }
            }
        }
        if (last_row) {
            const auto found = std::find(sequence.begin(), sequence.end(), *last_row);
            cursor_position = found == sequence.end()
                                  ? std::nullopt
                                  : std::optional<std::size_t>(
                                        static_cast<std::size_t>(found - sequence.begin()));
        }
    }

    std::optional<std::pair<RowId, std::vector<Value>>> read_position(std::size_t position) {
        const RowId id = sequence.at(position);
        detail::TableRowReader current(table->database->file, table_name(), table_definition());
        const auto candidate = current.read_at(id.page_number, id.row_number);
        return std::make_pair(id, decode(candidate));
    }

    bool current_matches(const RowPattern& pattern) {
        if (!last_row || at_end) {
            return false;
        }
        const auto resolved = resolve_pattern(pattern);
        detail::TableRowReader current(table->database->file, table_name(), table_definition());
        const auto candidate = current.read_at(last_row->page_number, last_row->row_number);
        return matches_pattern(candidate, resolved);
    }

    std::optional<std::pair<RowId, std::vector<Value>>> find_pattern(const RowPattern& pattern,
                                                                     bool from_beginning) {
        const auto resolved = resolve_pattern(pattern);
        const bool was_materialized = materialized;
        const auto saved_position = cursor_position;
        const bool saved_at_end = at_end;
        const auto saved_last_row = last_row;
        const auto restore = [&] {
            if (was_materialized) {
                cursor_position = saved_position;
            } else if (saved_last_row) {
                const auto found = std::find(sequence.begin(), sequence.end(), *saved_last_row);
                cursor_position = found == sequence.end()
                                      ? std::nullopt
                                      : std::optional<std::size_t>(
                                            static_cast<std::size_t>(found - sequence.begin()));
            } else {
                cursor_position.reset();
            }
            at_end = saved_at_end;
            last_row = saved_last_row;
        };

        try {
            materialize();
            const std::size_t start = from_beginning
                                          ? 0
                                          : (at_end ? sequence.size()
                                                    : (cursor_position ? *cursor_position + 1 : 0));
            detail::TableRowReader scan(table->database->file, table_name(), table_definition());
            for (std::size_t position = start; position < sequence.size(); ++position) {
                const RowId id = sequence[position];
                const auto candidate = scan.read_at(id.page_number, id.row_number);
                if (!matches_pattern(candidate, resolved)) {
                    continue;
                }
                cursor_position = position;
                at_end = false;
                last_row = id;
                return read_position(position);
            }
        } catch (...) {
            restore();
            throw;
        }
        restore();
        return std::nullopt;
    }

    std::shared_ptr<detail::TableState> table;
    std::optional<std::uint32_t> index_root;
    bool unique_index = false;
    std::optional<std::vector<std::uint8_t>> start_key;
    std::optional<std::vector<std::uint8_t>> stop_key;
    bool reverse = false;
    std::vector<std::uint16_t> projection;
    RowFilter filter;
    ErrorHandler inherited_error_handler;
    ErrorHandler error_handler;
    ColumnMatcher column_matcher;
    detail::TableRowReader reader;
    std::unique_ptr<detail::IndexEntryCursor> index_reader;
    std::vector<IndexPrefixPart> prefix;

    // Navigation state, filled once a backwards jump is requested.
    std::vector<RowId> sequence;
    bool materialized = false;
    std::optional<std::size_t> cursor_position;
    bool at_end = false;
    std::optional<RowId> last_row;
};

Row::Row(RowId id, std::vector<Value> values) : id_(id), values_(std::move(values)) {}

const RowId& Row::id() const noexcept {
    return id_;
}

std::size_t Row::size() const noexcept {
    return values_.size();
}

const std::vector<Value>& Row::values() const noexcept {
    return values_;
}

const Value& Row::at(std::size_t column_index) const {
    return values_.at(column_index);
}

Row RowCursor::make_row(RowId id, std::vector<Value> values) {
    return Row(id, std::move(values));
}

RowCursor::RowCursor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RowCursor::~RowCursor() = default;

RowCursor::RowCursor(RowCursor&& other) noexcept = default;

RowCursor& RowCursor::operator=(RowCursor&& other) noexcept = default;

std::optional<Row> RowCursor::next() {
    if (!impl_) {
        return std::nullopt;
    }
    if (impl_->at_end) {
        return std::nullopt;
    }
    if (impl_->materialized) {
        const std::size_t position = impl_->cursor_position ? *impl_->cursor_position + 1 : 0;
        if (position >= impl_->sequence.size()) {
            impl_->at_end = true;
            return std::nullopt;
        }
        impl_->cursor_position = position;
        auto entry = impl_->read_position(position);
        impl_->last_row = entry->first;
        return make_row(entry->first, std::move(entry->second));
    }
    while (auto candidate = impl_->advance()) {
        if (!impl_->accepts(*candidate)) {
            continue;
        }
        auto values = impl_->decode(*candidate);
        const RowId id{candidate->page_number, candidate->row_number};
        impl_->last_row = id;
        return make_row(id, std::move(values));
    }
    return std::nullopt;
}

std::optional<Row> RowCursor::previous() {
    if (!impl_) {
        return std::nullopt;
    }
    impl_->materialize();
    if (impl_->sequence.empty()) {
        return std::nullopt;
    }
    std::optional<std::size_t> position;
    if (impl_->at_end) {
        position = impl_->sequence.size() - 1;
    } else if (impl_->cursor_position && *impl_->cursor_position > 0) {
        position = *impl_->cursor_position - 1;
    } else {
        return std::nullopt;
    }
    impl_->at_end = false;
    impl_->cursor_position = position;
    auto entry = impl_->read_position(*position);
    impl_->last_row = entry->first;
    return make_row(entry->first, std::move(entry->second));
}

std::optional<Row> RowCursor::first() {
    if (!impl_) {
        return std::nullopt;
    }
    impl_->materialize();
    if (impl_->sequence.empty()) {
        return std::nullopt;
    }
    impl_->at_end = false;
    impl_->cursor_position = 0;
    auto entry = impl_->read_position(0);
    impl_->last_row = entry->first;
    return make_row(entry->first, std::move(entry->second));
}

std::optional<Row> RowCursor::last() {
    if (!impl_) {
        return std::nullopt;
    }
    impl_->materialize();
    if (impl_->sequence.empty()) {
        return std::nullopt;
    }
    const std::size_t position = impl_->sequence.size() - 1;
    impl_->at_end = false;
    impl_->cursor_position = position;
    auto entry = impl_->read_position(position);
    impl_->last_row = entry->first;
    return make_row(entry->first, std::move(entry->second));
}

std::optional<Row> RowCursor::move_to(RowId row_id) {
    if (!impl_) {
        return std::nullopt;
    }
    impl_->materialize();
    const auto found = std::find(impl_->sequence.begin(), impl_->sequence.end(), row_id);
    if (found == impl_->sequence.end()) {
        return std::nullopt;
    }
    const auto position = static_cast<std::size_t>(found - impl_->sequence.begin());
    impl_->at_end = false;
    impl_->cursor_position = position;
    auto entry = impl_->read_position(position);
    impl_->last_row = entry->first;
    return make_row(entry->first, std::move(entry->second));
}

std::optional<RowId> RowCursor::position() const {
    if (!impl_) {
        return std::nullopt;
    }
    return impl_->last_row;
}

std::optional<Row> RowCursor::current() {
    if (!impl_ || impl_->at_end || !impl_->cursor_position) {
        return std::nullopt;
    }
    if (!impl_->materialized) {
        // Without a materialized sequence the current row is still the last one returned.
        if (!impl_->last_row) {
            return std::nullopt;
        }
        detail::TableRowReader current(impl_->table->database->file, impl_->table_name(),
                                       impl_->table_definition());
        const auto candidate = current.read_at(impl_->last_row->page_number,
                                               impl_->last_row->row_number);
        return make_row(*impl_->last_row, impl_->decode(candidate));
    }
    auto entry = impl_->read_position(*impl_->cursor_position);
    return make_row(entry->first, std::move(entry->second));
}

bool RowCursor::current_row_matches(const RowPattern& pattern) {
    return impl_ && impl_->current_matches(pattern);
}

std::optional<Row> RowCursor::find_first_row(const RowPattern& pattern) {
    if (!impl_) {
        return std::nullopt;
    }
    auto match = impl_->find_pattern(pattern, true);
    if (!match) {
        return std::nullopt;
    }
    return make_row(match->first, std::move(match->second));
}

std::optional<Row> RowCursor::find_next_row(const RowPattern& pattern) {
    if (!impl_) {
        return std::nullopt;
    }
    auto match = impl_->find_pattern(pattern, false);
    if (!match) {
        return std::nullopt;
    }
    return make_row(match->first, std::move(match->second));
}

void RowCursor::set_error_handler(ErrorHandler handler) {
    if (!impl_) {
        return;
    }
    impl_->error_handler = handler ? std::move(handler) : impl_->inherited_error_handler;
}

void RowCursor::set_column_matcher(ColumnMatcher matcher) {
    if (impl_) {
        impl_->column_matcher = std::move(matcher);
    }
}

namespace {

const IndexInfo& require_index(const detail::TableState& state, const std::string& index_name) {
    const auto& indexes = state.database->indexes.at(state.schema_index);
    const auto index = std::find_if(indexes.begin(), indexes.end(), [&](const IndexInfo& candidate) {
        return candidate.name == index_name;
    });
    if (index == indexes.end()) {
        throw std::invalid_argument("Table has no index named '" + index_name + "'");
    }
    return *index;
}

std::vector<IndexPrefixPart> encode_index_parts(const detail::TableState& state,
                                                const IndexInfo& index,
                                                const std::vector<Value>& values,
                                                bool allow_prefix = false) {
    if (values.empty() || values.size() > index.columns.size() ||
        (!allow_prefix && values.size() != index.columns.size())) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Index key has the wrong number of column values",
                            state.database->file.path());
    }
    std::vector<IndexPrefixPart> parts;
    parts.reserve(values.size());
    try {
        for (std::size_t number = 0; number < values.size(); ++number) {
            const auto& indexed = index.columns[number];
            const auto& column = resolve_index_column(state, indexed);
            parts.push_back({&column, !indexed.ascending,
                             detail::encode_jet4_index_key(column, values[number], !indexed.ascending)});
        }
    } catch (const std::invalid_argument& error) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation, error.what(),
                            state.database->file.path());
    }
    return parts;
}

std::vector<std::uint8_t> concatenate_index_parts(const std::vector<IndexPrefixPart>& parts) {
    std::vector<std::uint8_t> key;
    for (const auto& part : parts) {
        key.insert(key.end(), part.key.begin(), part.key.end());
    }
    return key;
}

std::vector<std::uint8_t> encode_index_bound(const detail::TableState& state, const IndexInfo& index,
                                             const std::vector<Value>& values) {
    return detail::truncate_jet4_index_key(concatenate_index_parts(encode_index_parts(state, index, values)));
}

std::optional<std::vector<std::uint8_t>> after_index_prefix(std::vector<std::uint8_t> key) {
    while (!key.empty() && key.back() == 0xff) {
        key.pop_back();
    }
    if (key.empty()) {
        return std::nullopt;
    }
    ++key.back();
    return key;
}

}

Table::Table(std::shared_ptr<detail::TableState> state) : state_(std::move(state)) {}

const std::string& Table::name() const {
    const auto& state = checked_state(state_);
    return state.database->schema.tables[state.schema_index].name;
}

const std::string& Table::object_type() const {
    const auto& state = checked_state(state_);
    return state.database->schema.tables[state.schema_index].object_type;
}

bool Table::is_user_table() const {
    const auto& state = checked_state(state_);
    return state.database->schema.tables[state.schema_index].is_user_table;
}

bool Table::is_linked() const {
    const auto& state = checked_state(state_);
    return state.database->schema.tables[state.schema_index].is_linked;
}

std::uint32_t Table::row_count_header() const {
    const auto& state = checked_state(state_);
    return state.database->schema.tables[state.schema_index].definition.row_count;
}

const std::vector<ColumnInfo>& Table::columns() const {
    const auto& state = checked_state(state_);
    return state.database->columns[state.schema_index];
}

const std::vector<IndexInfo>& Table::indexes() const {
    const auto& table = checked_state(state_);
    return table.database->indexes.at(table.schema_index);
}

void Table::set_error_handler(ErrorHandler handler) {
    const auto& table = checked_state(state_);
    std::lock_guard<std::recursive_mutex> lock(table.database->write_mutex);
    state_->error_handler = std::move(handler);
}

namespace {

std::vector<PropertyMapInfo> object_property_maps(const detail::PageFile& file,
                                                  const std::string& name,
                                                  std::int16_t type,
                                                  std::optional<std::uint64_t> id) {
    if (file.format().version != EngineVersion::jet4) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Property maps are currently supported only for Jet4 databases", file.path());
    }
    const auto catalog = detail::read_table_definition(file, 2);
    const auto objects = detail::read_system_catalog_objects(file, catalog, name);
    for (const auto& object : objects) {
        if (object.name != name || object.type != type ||
            (id && (!object.id || *object.id < 0 || static_cast<std::uint64_t>(*object.id) != *id))) {
            continue;
        }
        if (object.property_error) {
            DatabaseErrorContext context;
            context.table_name = name;
            context.row_number = object.row_number;
            context.column_name = "LvProp";
            throw DatabaseError(DatabaseErrorCode::corrupted_file, *object.property_error, file.path(),
                                object.property_error_page, std::move(context));
        }
        if (object.property_bytes) {
            auto maps = detail::read_property_maps(file, name, object.page_number, object.row_number,
                                                   *object.property_bytes);
            detail::decode_property_values(file, name, object.page_number, object.row_number, maps);
            return maps;
        }
        return {};
    }
    return {};
}

}

std::vector<PropertyMapInfo> Table::property_maps() const {
    const auto& state = checked_state(state_);
    const auto& table = state.database->schema.tables.at(state.schema_index);
    return object_property_maps(state.database->file, table.name, 1, table.definition_page_number);
}

std::vector<PropertyMapInfo> Database::property_maps() const {
    return object_property_maps(checked_state(state_).file, "MSysDb", 2, std::nullopt);
}

namespace {

void save_object_property_maps(detail::DatabaseState& state, const std::string& name,
                               std::int16_t type, std::optional<std::uint64_t> id,
                               const std::vector<std::uint8_t>& bytes) {
    if (!state.writable) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Saving property maps requires a writable Jet4 database", state.file.path());
    }
    const auto& tables = state.schema.tables;
    const auto catalog = std::find_if(tables.begin(), tables.end(), [](const auto& table) {
        return table.definition_page_number == 2 && !table.is_user_table;
    });
    if (catalog == tables.end()) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "The MSysObjects catalog is unavailable", state.file.path());
    }
    const auto objects = detail::read_system_catalog_objects(state.file, catalog->definition, name);
    const auto object = std::find_if(objects.begin(), objects.end(), [&](const auto& item) {
        return item.name == name && item.type == type &&
               (!id || (item.id && *item.id >= 0 && static_cast<std::uint64_t>(*item.id) == *id));
    });
    if (object == objects.end() || !object->property_column_present) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Object property storage is unavailable", state.file.path());
    }
    detail::Jet4WriteContext context{state.file, state.schema, state.writable,
                                     state.write_state_valid, state.write_mutex};
    detail::update_jet4_catalog_properties(context, static_cast<std::size_t>(catalog - tables.begin()),
                                          {object->page_number, object->row_number}, bytes);
}

}

void Database::save_property_maps(const std::vector<PropertyMapInfo>& maps) {
    if (!state_) {
        throw std::logic_error("Database has no state; it may have been moved from");
    }
    std::lock_guard<std::recursive_mutex> lock(state_->write_mutex);
    save_object_property_maps(*state_, "MSysDb", 2, std::nullopt, detail::encode_property_maps(maps));
}

void Database::set_column_validator_factory(ColumnValidatorFactory factory) {
    if (!state_) {
        throw std::logic_error("Database has no state; it may have been moved from");
    }
    std::lock_guard<std::recursive_mutex> lock(state_->write_mutex);
    std::vector<std::vector<ColumnValidator>> validators;
    validators.reserve(state_->schema.tables.size());
    for (std::size_t table_index = 0; table_index < state_->schema.tables.size(); ++table_index) {
        const auto& table = state_->schema.tables[table_index];
        std::vector<ColumnValidator> table_validators;
        table_validators.reserve(state_->columns[table_index].size());
        for (const auto& column : state_->columns[table_index]) {
            table_validators.push_back(factory ? factory(table.name, column) : ColumnValidator{});
        }
        validators.push_back(std::move(table_validators));
    }
    state_->column_validators = std::move(validators);
}

void Database::set_error_handler(ErrorHandler handler) {
    if (!state_) {
        throw std::logic_error("Database has no state; it may have been moved from");
    }
    std::lock_guard<std::recursive_mutex> lock(state_->write_mutex);
    state_->error_handler = std::move(handler);
}

void Table::save_property_maps(const std::vector<PropertyMapInfo>& maps) {
    const auto& state = checked_state(state_);
    auto& database = *state.database;
    std::lock_guard<std::recursive_mutex> lock(database.write_mutex);
    auto& table = database.schema.tables.at(state.schema_index);
    if (!database.writable || !table.is_user_table || table.is_linked) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Saving table properties requires a writable local user table", database.file.path());
    }

    const auto bytes = detail::encode_property_maps(maps);
    std::vector<std::string> names;
    names.reserve(table.definition.columns.size());
    for (const auto& column : table.definition.columns) {
        names.push_back(column.name);
    }
    const auto nullable = detail::read_column_nullability(database.file, table.name,
                                                           table.definition_page_number, 0, names, bytes);
    const auto property = [&](std::string_view name) {
        return detail::read_column_text_properties(database.file, table.name,
                                                   table.definition_page_number, 0, names, bytes, name);
    };
    auto defaults = property("DefaultValue");
    auto rules = property("ValidationRule");
    auto messages = property("ValidationText");
    const auto table_validation_rule = detail::read_text_property(
        database.file, table.name, table.definition_page_number, 0, bytes, "ValidationRule");
    const auto table_validation_text = detail::read_text_property(
        database.file, table.name, table.definition_page_number, 0, bytes, "ValidationText");
    save_object_property_maps(database, table.name, 1, table.definition_page_number, bytes);
    for (std::size_t i = 0; i < nullable.size(); ++i) {
        table.definition.columns[i].nullable = nullable[i];
        table.definition.columns[i].default_expression = std::move(defaults[i]);
        table.definition.columns[i].validation_rule = std::move(rules[i]);
        table.definition.columns[i].validation_text = std::move(messages[i]);
        database.columns[state.schema_index][i].nullable = nullable[i];
    }
    table.definition.table_validation_rule = table_validation_rule;
    table.definition.table_validation_text = table_validation_text;
    table.definition.column_properties_available = true;
}

namespace {

// Maps column names to their position in the table definition; the comparison ignores ASCII case
// like the rest of the catalog lookups.
std::vector<std::uint16_t> resolve_projection(const detail::TableState& state,
                                              const std::vector<std::string>& column_names) {
    const auto& definition = state.database->schema.tables.at(state.schema_index).definition;
    std::vector<std::uint16_t> projection;
    projection.reserve(column_names.size());
    for (const auto& name : column_names) {
        const auto found = std::find_if(definition.columns.begin(), definition.columns.end(),
                                        [&](const auto& column) {
            return column.name.size() == name.size() &&
                   std::equal(column.name.begin(), column.name.end(), name.begin(),
                              [](char left, char right) {
                                  return (left >= 'A' && left <= 'Z' ? left + 32 : left) ==
                                         (right >= 'A' && right <= 'Z' ? right + 32 : right);
                              });
        });
        if (found == definition.columns.end()) {
            throw std::invalid_argument("Table has no column named '" + name + "'");
        }
        projection.push_back(static_cast<std::uint16_t>(found - definition.columns.begin()));
    }
    if (projection.empty()) {
        throw std::invalid_argument("A column projection needs at least one column name");
    }
    return projection;
}

}  // namespace

RowCursor Table::rows() const {
    checked_state(state_);
    return RowCursor(std::make_unique<RowCursor::Impl>(state_));
}

RowCursor Table::rows(const std::vector<std::string>& column_names, RowFilter filter) const {
    const auto& state = checked_state(state_);
    return RowCursor(std::make_unique<RowCursor::Impl>(state_, std::nullopt, false,
                                                       std::nullopt, std::nullopt, false,
                                                       resolve_projection(state, column_names),
                                                       std::move(filter)));
}

RowCursor Table::rows_by_index(const std::string& index_name) const {
    const auto& state = checked_state(state_);
    const auto& indexes = state.database->indexes.at(state.schema_index);
    const auto index = std::find_if(indexes.begin(), indexes.end(), [&](const IndexInfo& candidate) {
        return candidate.name == index_name;
    });
    if (index == indexes.end()) {
        throw std::invalid_argument("Table has no index named '" + index_name + "'");
    }
    return RowCursor(std::make_unique<RowCursor::Impl>(state_, index->root_page_number,
                                                       index->unique));
}

RowCursor Table::rows_by_index(const std::string& index_name,
                               const std::vector<std::string>& column_names,
                               RowFilter filter) const {
    const auto& state = checked_state(state_);
    const auto& index = require_index(state, index_name);
    return RowCursor(std::make_unique<RowCursor::Impl>(state_, index.root_page_number,
                                                       index.unique, std::nullopt, std::nullopt, false,
                                                       resolve_projection(state, column_names),
                                                       std::move(filter)));
}

RowCursor Table::rows_by_index_from(const std::string& index_name, const Value& start) const {
    return rows_by_index_from(index_name, std::vector<Value>{start});
}

RowCursor Table::rows_by_index_from(const std::string& index_name, const std::vector<Value>& start) const {
    const auto& state = checked_state(state_);
    const auto& index = require_index(state, index_name);
    auto start_key = encode_index_bound(state, index, start);
    return RowCursor(std::make_unique<RowCursor::Impl>(state_,
                                                       index.root_page_number,
                                                       index.unique,
                                                       std::move(start_key),
                                                       std::nullopt));
}

RowCursor Table::rows_by_index_range(const std::string& index_name,
                                     const Value& start,
                                     const Value& stop) const {
    return rows_by_index_range(index_name, std::vector<Value>{start}, std::vector<Value>{stop});
}

RowCursor Table::rows_by_index_range(const std::string& index_name,
                                     const std::vector<Value>& start,
                                     const std::vector<Value>& stop) const {
    const auto& state = checked_state(state_);
    const auto& index = require_index(state, index_name);
    auto start_key = encode_index_bound(state, index, start);
    auto stop_key = encode_index_bound(state, index, stop);
    return RowCursor(std::make_unique<RowCursor::Impl>(state_,
                                                       index.root_page_number,
                                                       index.unique,
                                                       std::move(start_key),
                                                       std::move(stop_key)));
}

RowCursor Table::rows_by_index_reverse(const std::string& index_name) const {
    const auto& state = checked_state(state_);
    const auto& indexes = state.database->indexes.at(state.schema_index);
    const auto index = std::find_if(indexes.begin(), indexes.end(), [&](const IndexInfo& candidate) {
        return candidate.name == index_name;
    });
    if (index == indexes.end()) {
        throw std::invalid_argument("Table has no index named '" + index_name + "'");
    }
    return RowCursor(
        std::make_unique<RowCursor::Impl>(state_, index->root_page_number, index->unique,
                                          std::nullopt, std::nullopt, true));
}

RowCursor Table::rows_by_index_range_reverse(const std::string& index_name,
                                             const Value& start,
                                             const Value& stop) const {
    return rows_by_index_range_reverse(index_name, std::vector<Value>{start}, std::vector<Value>{stop});
}

RowCursor Table::rows_by_index_range_reverse(const std::string& index_name,
                                             const std::vector<Value>& start,
                                             const std::vector<Value>& stop) const {
    const auto& state = checked_state(state_);
    const auto& index = require_index(state, index_name);
    auto start_key = encode_index_bound(state, index, start);
    auto stop_key = encode_index_bound(state, index, stop);
    return RowCursor(std::make_unique<RowCursor::Impl>(state_,
                                                       index.root_page_number,
                                                       index.unique,
                                                       std::move(start_key),
                                                       std::move(stop_key),
                                                       true));
}

RowCursor Table::rows_by_index_prefix(const std::string& index_name, const std::vector<Value>& prefix,
                                      bool reverse) const {
    const auto& state = checked_state(state_);
    const auto& index = require_index(state, index_name);
    auto parts = encode_index_parts(state, index, prefix, true);
    auto start = concatenate_index_parts(parts);
    if (parts.size() == index.columns.size()) {
        start = detail::truncate_jet4_index_key(std::move(start));
    } else if (start.size() > detail::jet4_max_index_key_length - 2) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "An index prefix longer than 508 encoded bytes cannot be scanned safely",
                            state.database->file.path());
    }
    auto stop = after_index_prefix(start);
    auto cursor = std::make_unique<RowCursor::Impl>(state_, index.root_page_number,
                                                   index.unique, std::move(start),
                                                   std::move(stop), reverse);
    cursor->prefix = std::move(parts);
    return RowCursor(std::move(cursor));
}

std::optional<Row> Table::find_row_by_index(const std::string& index_name,
                                            const Value& key) const {
    return find_row_by_index(index_name, std::vector<Value>{key});
}

std::optional<Row> Table::find_row_by_index(const std::string& index_name,
                                            const std::vector<Value>& key) const {
    const auto& state = checked_state(state_);
    const auto& table = state.database->schema.tables[state.schema_index];
    const auto& index = require_index(state, index_name);
    const auto encoded_key = encode_index_bound(state, index, key);

    detail::IndexEntryCursor cursor(state.database->file,
                                    table.name,
                                    table.definition_page_number,
                                    index.root_page_number,
                                    encoded_key,
                                    std::nullopt);
    const auto entry = cursor.next_entry();
    if (!entry || entry->key != encoded_key) {
        return std::nullopt;
    }

    detail::TableRowReader reader(state.database->file, table.name, table.definition);
    const auto encoded_row = reader.read_at(entry->row.page_number, entry->row.row_number);
    RowCursor::Impl decoder(state_);
    return Row({entry->row.page_number, entry->row.row_number}, decoder.decode(encoded_row));
}

RowId Table::add_row(const std::vector<Value>& values) {
    const auto row_ids = add_rows({values});
    return row_ids.front();
}

std::vector<RowId> Table::add_rows(const std::vector<std::vector<Value>>& rows) {
    const auto& state = checked_state(state_);
    detail::Jet4WriteContext context{state.database->file,
                                     state.database->schema,
                                     state.database->writable,
                                     state.database->write_state_valid,
                                     state.database->write_mutex,
                                     nullptr,
                                     &state.database->columns,
                                     &state.database->column_validators};
    return detail::append_jet4_rows(context, state.schema_index, rows);
}

void Table::update_row(RowId row_id, const std::vector<Value>& values) {
    const auto& state = checked_state(state_);
    detail::Jet4WriteContext context{state.database->file,
                                     state.database->schema,
                                     state.database->writable,
                                     state.database->write_state_valid,
                                     state.database->write_mutex,
                                     nullptr,
                                     &state.database->columns,
                                     &state.database->column_validators};
    detail::run_jet4_write_operation(context, [&] {
        detail::update_jet4_row(context, state.schema_index, row_id, values);
    });
}

void Table::delete_row(RowId row_id) {
    const auto& state = checked_state(state_);
    detail::Jet4WriteContext context{state.database->file,
                                     state.database->schema,
                                     state.database->writable,
                                     state.database->write_state_valid,
                                     state.database->write_mutex};
    detail::run_jet4_write_operation(context, [&] {
        detail::delete_jet4_row(context, state.schema_index, row_id);
    });
}

void Table::add_index(const IndexBuilder& index) {
    const auto& state = checked_state(state_);
    auto& database = *state.database;
    detail::Jet4WriteContext context{database.file, database.schema, database.writable,
                                     database.write_state_valid, database.write_mutex};
    detail::run_jet4_write_operation(context, [&] {
        auto definition = detail::add_jet4_index(context, state.schema_index, index);
        auto indexes = public_indexes(definition);
        database.indexes[state.schema_index] = std::move(indexes);
        database.schema.tables[state.schema_index].definition = std::move(definition);
    });
}

void Database::create_relationship(const RelationshipBuilder& relationship) {
    if (!state_) {
        throw std::logic_error("Database has no state; it may have been moved from");
    }
    std::lock_guard<std::recursive_mutex> lock(state_->write_mutex);
    detail::Jet4WriteContext context{state_->file,
                                     state_->schema,
                                     state_->writable,
                                     state_->write_state_valid,
                                     state_->write_mutex};
    detail::RelationshipDefinition definition;
    if (context.writable && context.write_state_valid) {
        definition.name = detail::prepare_relationship_name(*this, relationship);
    }
    definition.from_table = relationship.from_table();
    definition.to_table = relationship.to_table();
    definition.referential_integrity = relationship.referential_integrity();
    definition.cascade_updates = relationship.cascade_updates();
    definition.cascade_deletes = relationship.cascade_deletes();
    definition.cascade_null_on_delete = relationship.cascade_null_on_delete();
    definition.join_type = relationship.join_type();
    definition.columns.reserve(relationship.columns().size());
    for (const auto& column : relationship.columns()) {
        definition.columns.emplace_back(column.from_column, column.to_column);
    }
    detail::run_jet4_write_operation(context, [&] {
        const auto changed_tables = detail::create_jet4_relationship(context, definition);
        std::vector<detail::TableDefinition> updated_definitions;
        std::vector<std::vector<IndexInfo>> updated_indexes;
        updated_definitions.reserve(changed_tables.size());
        updated_indexes.reserve(changed_tables.size());
        for (const auto position : changed_tables) {
            auto& table = state_->schema.tables[position];
            auto updated = detail::read_table_definition(state_->file, table.definition_page_number);
            for (std::size_t column = 0; column < updated.columns.size(); ++column) {
                const auto& previous = table.definition.columns.at(column);
                updated.columns[column].nullable = previous.nullable;
                updated.columns[column].default_expression = previous.default_expression;
                updated.columns[column].validation_rule = previous.validation_rule;
                updated.columns[column].validation_text = previous.validation_text;
            }
            updated.column_properties_available = table.definition.column_properties_available;
            updated_indexes.push_back(public_indexes(updated));
            updated_definitions.push_back(std::move(updated));
        }
        for (std::size_t i = 0; i < changed_tables.size(); ++i) {
            const auto position = changed_tables[i];
            state_->indexes[position] = std::move(updated_indexes[i]);
            state_->schema.tables[position].definition = std::move(updated_definitions[i]);
        }
    });
}

Database::Database(std::shared_ptr<detail::DatabaseState> state) : state_(std::move(state)) {}

Database Database::open(const std::filesystem::path& path) {
    return open(path, DatabaseOpenMode::read_only);
}

Database Database::open(const std::filesystem::path& path, DatabaseOpenMode mode) {
    if (mode != DatabaseOpenMode::read_only && mode != DatabaseOpenMode::read_write) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Unknown database open mode",
                            path);
    }
    std::optional<detail::PageFileWriteLock> write_lock;
    if (mode == DatabaseOpenMode::read_write) {
        write_lock.emplace(detail::PageFileWriteLock::acquire(path));
    }
    if (detail::WriteJournal::pending(path)) {
        if (mode != DatabaseOpenMode::read_write) {
            throw DatabaseError(DatabaseErrorCode::io_error,
                                "A previous write did not finish; reopen the database read-write to roll it back",
                                path);
        }
        // The journal holds the pre-images of a batch that never reached a flush. Replaying it
        // restores the pages and removes pages the batch appended.
        detail::WriteJournal::recover(path);
    }
    auto file = detail::PageFile::open(path);
    const auto engine_version = file.format().version;
    if (mode == DatabaseOpenMode::read_write &&
        (engine_version == EngineVersion::ace2007 || engine_version == EngineVersion::ace2010 ||
         engine_version == EngineVersion::ace2016 || engine_version == EngineVersion::ace2019)) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Read-write access is not implemented for ACE databases",
                            path);
    }
    auto schema = detail::read_table_schema_catalog(file);
    return Database(std::make_shared<detail::DatabaseState>(std::move(file),
                                                            std::move(schema),
                                                            mode == DatabaseOpenMode::read_write,
                                                            std::move(write_lock)));
}

Database Database::open(const std::vector<std::uint8_t>& bytes) {
    auto file = detail::PageFile::open(bytes);
    auto schema = detail::read_table_schema_catalog(file);
    return Database(std::make_shared<detail::DatabaseState>(std::move(file),
                                                            std::move(schema),
                                                            false,
                                                            std::nullopt));
}

Database Database::create(const std::filesystem::path& path, DatabaseFormat format) {
    return create(path, format, {});
}

Database Database::create(const std::filesystem::path& path,
                           DatabaseFormat format,
                           const std::vector<TableBuilder>& tables) {
    if (!tables.empty() && format != DatabaseFormat::v2000 && format != DatabaseFormat::v2003) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Initial table schema creation currently supports Jet4 V2000 and V2003 files only",
                            path);
    }

    if (detail::WriteJournal::pending(path)) {
        // The destination is replaced wholesale, so pre-images recorded for the old file must not
        // be replayed into the new one.
        std::error_code ignored;
        std::filesystem::remove(detail::WriteJournal::path_for(path), ignored);
    }

    std::optional<detail::PageFileWriteLock> replacement_lock;
    std::error_code existence_error;
    if (std::filesystem::exists(path, existence_error)) {
        replacement_lock.emplace(detail::PageFileWriteLock::acquire_for_replacement(path));
    } else if (existence_error) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Cannot determine whether the database destination exists: " +
                                existence_error.message(),
                            path);
    }

    auto staged = detail::stage_jet4_database(path, format);
    for (const auto& table : tables) {
        detail::create_jet4_table(staged.temporary_path(), format, table);
    }

    {
        auto validation_database = Database::open(staged.temporary_path());
        const auto created_tables = validation_database.tables();
        for (const auto& expected : tables) {
            const auto created = std::find_if(created_tables.begin(), created_tables.end(), [&](const Table& candidate) {
                return candidate.name() == expected.name();
            });
            if (created == created_tables.end() || !created->is_user_table() ||
                created->columns().size() != expected.columns().size()) {
                throw DatabaseError(DatabaseErrorCode::corrupted_file,
                                    "Staged database does not contain the requested table schema",
                                    staged.temporary_path());
            }
            for (std::size_t index = 0; index < expected.columns().size(); ++index) {
                if (created->columns()[index].name != expected.columns()[index].name() ||
                    created->columns()[index].type != expected.columns()[index].type()) {
                    throw DatabaseError(DatabaseErrorCode::corrupted_file,
                                        "Staged database table columns do not match the requested schema",
                                        staged.temporary_path());
                }
            }
            const auto& indexes = created->indexes();
            if (!std::equal(indexes.begin(), indexes.end(),
                            expected.indexes().begin(), expected.indexes().end(),
                            [](const IndexInfo& actual, const IndexBuilder& requested) {
                                return actual.name == requested.name() &&
                                       actual.unique == requested.unique() &&
                                       actual.primary_key == requested.primary_key() &&
                                       actual.required == requested.required() &&
                                       actual.ignore_nulls == requested.ignore_nulls() &&
                                       std::equal(actual.columns.begin(), actual.columns.end(),
                                                  requested.columns().begin(), requested.columns().end(),
                                                  [](const IndexColumnInfo& actual_column,
                                                     const IndexColumnBuilder& requested_column) {
                                                      return actual_column.name == requested_column.name &&
                                                             actual_column.ascending == requested_column.ascending;
                                                  });
                            })) {
                throw DatabaseError(DatabaseErrorCode::corrupted_file,
                                    "Staged database indexes do not match the requested schema",
                                    staged.temporary_path());
            }
            if (created->rows().next()) {
                throw DatabaseError(DatabaseErrorCode::corrupted_file,
                                    "Newly created table unexpectedly contains rows",
                                    staged.temporary_path());
            }
        }
    }
    staged.publish();
    return Database::open(path);
}

const std::filesystem::path& Database::path() const {
    return checked_state(state_).file.path();
}

const FormatInfo& Database::format() const {
    return checked_state(state_).file.format();
}

FileFormat Database::file_format() const {
    return detail::identify_file_format(checked_state(state_).file);
}

const std::vector<DatabaseDiagnostic>& Database::diagnostics() const {
    return checked_state(state_).diagnostics;
}

std::vector<LinkedTableInfo> Database::linked_tables() const {
    const auto& file = checked_state(state_).file;
    const auto catalog = detail::read_table_definition(file, 2);
    auto objects = detail::read_system_catalog_objects(file, catalog, std::string_view{}, true);
    std::vector<LinkedTableInfo> links;
    for (auto& object : objects) {
        if (object.name && (object.type == 4 || object.type == 6)) {
            links.push_back({std::move(*object.name),
                             object.type == 4 ? LinkedTableKind::odbc : LinkedTableKind::file,
                             std::move(object.linked_database), std::move(object.linked_table),
                             std::move(object.connection_string)});
        }
    }
    return links;
}

void Database::create_linked_table(std::string name, std::string target_database, std::string target_table) {
    if (!state_) {
        throw std::logic_error("Database has no state; it may have been moved from");
    }
    std::lock_guard<std::recursive_mutex> lock(state_->write_mutex);
    const auto& file = state_->file;
    const auto reject = [&](const char* message) {
        return DatabaseError(DatabaseErrorCode::unsupported_operation, message, file.path());
    };
    if (!state_->writable || file.format().version != EngineVersion::jet4) {
        throw reject("Creating a link requires a writable Jet4 database");
    }
    if (name.empty() || target_database.empty() || target_table.empty()) {
        throw std::invalid_argument("Link name, database and target table are required");
    }

    const auto& tables = state_->schema.tables;
    const auto catalog = std::find_if(tables.begin(), tables.end(), [](const auto& table) {
        return table.definition_page_number == 2 && !table.is_user_table;
    });
    if (catalog == tables.end()) {
        throw reject("The MSysObjects catalog is unavailable");
    }
    const auto objects = detail::read_system_catalog_objects(file, catalog->definition, std::string_view{});
    const auto container = std::find_if(objects.begin(), objects.end(), [](const auto& object) {
        return object.name == "Tables" && object.type == 3 && object.id;
    });
    if (container == objects.end()) {
        throw reject("The system catalog has no Tables container");
    }
    const auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; };
    auto max_id = std::numeric_limits<std::int32_t>::min();
    for (const auto& object : objects) {
        if (object.id && *object.id < 0) {
            max_id = std::max(max_id, *object.id);
        }
        if (object.parent_id == container->id && object.name &&
            std::equal(name.begin(), name.end(), object.name->begin(), object.name->end(),
                       [&](char lhs, char rhs) { return fold(lhs) == fold(rhs); })) {
            throw reject("A catalog object with that table name already exists");
        }
    }
    if (max_id == -1) {
        throw reject("There are no remaining synthetic table identifiers");
    }
    const auto column_for = [&](const detail::TableDefinition& definition,
                                std::string_view field) -> const detail::ColumnDefinition& {
        const auto column = std::find_if(definition.columns.begin(), definition.columns.end(),
                                         [&](const auto& item) { return item.name == field; });
        if (column == definition.columns.end()) {
            throw reject("The system catalog is missing a link metadata field");
        }
        return *column;
    };
    const auto access = std::find_if(tables.begin(), tables.end(), [](const auto& table) {
        return table.name == "MSysACEs" && !table.is_user_table;
    });
    if (access == tables.end()) {
        throw reject("The MSysACEs catalog is unavailable");
    }
    const auto& access_id = column_for(access->definition, "ObjectId");
    const auto& access_sid = column_for(access->definition, "SID");
    const auto& access_mask = column_for(access->definition, "ACM");
    const auto& access_inherit = column_for(access->definition, "FInheritable");
    const auto object_id = max_id + 1;
    const BlobValue default_sid{{0xa6, 0x33}};
    std::vector<std::vector<Value>> access_rows;
    const auto add_access = [&](BlobValue sid) {
        std::vector<Value> values(access->definition.columns.size());
        values.at(access_id.column_number) = object_id;
        values.at(access_sid.column_number) = std::move(sid);
        values.at(access_mask.column_number) = std::int32_t{1048575};
        values.at(access_inherit.column_number) = false;
        access_rows.push_back(std::move(values));
    };
    detail::TableRowReader reader(file, access->name, access->definition);
    while (const auto row = reader.next()) {
        const auto id = detail::decode_jet4_value(file, access->name, *row, access_id);
        const auto* number = std::get_if<std::int32_t>(&id);
        if (!number || *number != *container->id) {
            continue;
        }
        auto sid = detail::decode_jet4_value(file, access->name, *row, access_sid);
        const auto* bytes = std::get_if<BlobValue>(&sid);
        if (!bytes) {
            throw reject("A Tables access control entry has no SID");
        }
        add_access(*bytes);
    }
    if (access_rows.empty()) {
        add_access(default_sid);
    }

    std::vector<Value> row(catalog->definition.columns.size());
    const auto set = [&](std::string_view field, Value value) {
        row.at(column_for(catalog->definition, field).column_number) = std::move(value);
    };
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto date_serial = 25569.0 + std::chrono::duration<double>(now).count() / 86400.0;
    set("Id", object_id);
    set("ParentId", *container->id);
    set("Name", std::move(name));
    set("Type", std::int16_t{6});
    set("Flags", std::int32_t{0});
    set("DateCreate", date_serial);
    set("DateUpdate", date_serial);
    set("Owner", default_sid);
    set("Database", MemoValue{std::move(target_database), {}});
    set("ForeignName", std::move(target_table));

    detail::Jet4WriteContext context{state_->file, state_->schema, state_->writable,
                                     state_->write_state_valid, state_->write_mutex};
    detail::run_jet4_write_operation(context, [&] {
        detail::append_jet4_catalog_rows(context, static_cast<std::size_t>(catalog - tables.begin()), {row});
        detail::append_jet4_catalog_rows(context, static_cast<std::size_t>(access - tables.begin()), access_rows);
    });
}

std::vector<Table> Database::tables() const {
    const auto& state = checked_state(state_);
    std::vector<Table> tables;
    tables.reserve(state.schema.tables.size());
    for (std::size_t index = 0; index < state.schema.tables.size(); ++index) {
        auto table_state = std::make_shared<detail::TableState>();
        table_state->database = state_;
        table_state->schema_index = index;
        tables.push_back(Table(std::move(table_state)));
    }
    return tables;
}

}
