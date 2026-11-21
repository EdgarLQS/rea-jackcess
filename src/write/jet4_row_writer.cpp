#include "jet4_row_writer.hpp"

#include "jet4_fixed_type.hpp"
#include "jet4_global_usage_map.hpp"
#include "jet4_variable_type.hpp"

#include "../util/little_endian.hpp"
#include "../catalog/property_map.hpp"
#include "../catalog/system_catalog.hpp"
#include "../catalog/table_definition.hpp"
#include "../catalog/row_reader.hpp"
#include "../catalog/usage_map.hpp"
#include "../page/data_page.hpp"
#include "../page/page_types.hpp"
#include "../page/page_writer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint64_t data_page_row_count_offset = 12;
constexpr std::uint64_t data_page_row_directory_offset = 14;
constexpr std::uint64_t data_page_table_definition_offset = 4;
constexpr std::uint16_t data_page_row_offset_mask = 0x1fff;
constexpr std::uint16_t data_page_deleted_row_mask = 0x8000;
constexpr std::uint16_t data_page_overflow_row_mask = 0x4000;
constexpr std::uint32_t long_value_length_mask = 0x3fffffffu;
constexpr std::uint32_t long_value_inline_flag = 0x80000000u;
constexpr std::uint32_t long_value_other_page_flag = 0x40000000u;
constexpr std::uint64_t jet4_v2000_max_long_value_row_size = 2032;
constexpr std::uint64_t long_value_page_pointer_size = 4;

std::uint32_t read_u24_le(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16);
}

void write_u24_le(std::vector<std::uint8_t>& bytes, std::uint64_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
}

[[noreturn]] void throw_table_error(DatabaseErrorCode code,
                                   const PageFile& file,
                                   const std::string& table_name,
                                   const char* message,
                                   std::optional<std::uint64_t> page_number = std::nullopt) {
    DatabaseErrorContext context;
    context.table_name = table_name;
    throw DatabaseError(code, message, file.path(), page_number, std::move(context));
}

[[noreturn]] void rethrow_with_table_context(const PageFile& file,
                                             const std::string& table_name,
                                             const DatabaseError& error,
                                             std::optional<std::uint64_t> row_number = std::nullopt) {
    auto context = error.context();
    if (!context.table_name) {
        context.table_name = table_name;
    }
    if (row_number && !context.row_number) {
        context.row_number = row_number;
    }
    throw DatabaseError(error.code(), error.what(), file.path(), error.page_number(), std::move(context));
}

[[noreturn]] void throw_row_error(DatabaseErrorCode code,
                                  const PageFile& file,
                                  const std::string& table_name,
                                  RowId row_id,
                                  const char* message) {
    DatabaseErrorContext context;
    context.table_name = table_name;
    context.row_number = row_id.row_number;
    throw DatabaseError(code, message, file.path(), row_id.page_number, std::move(context));
}

template <typename WriteOperation>
void write_pages_and_refresh(Jet4WriteContext& context,
                             const PageFile& file,
                             const std::string& table_name,
                             std::optional<std::uint64_t> row_number,
                             std::optional<std::uint64_t> page_number,
                             WriteOperation&& write_operation) {
    bool writes_started = false;
    try {
        PageFileWriter writer = PageFileWriter::open(file.path());
        if (writer.page_count() != file.page_count()) {
            throw_table_error(DatabaseErrorCode::io_error,
                              file,
                              table_name,
                              "Database page count changed before write pages were written",
                              page_number);
        }
        writes_started = true;
        std::forward<WriteOperation>(write_operation)(writer);
        writer.flush();
    } catch (const DatabaseError& error) {
        if (writes_started) {
            context.write_state_valid = false;
        }
        rethrow_with_table_context(file, table_name, error, row_number);
    } catch (...) {
        if (writes_started) {
            context.write_state_valid = false;
        }
        throw;
    }

    try {
        context.file = PageFile::open(file.path());
    } catch (const DatabaseError& error) {
        context.write_state_valid = false;
        rethrow_with_table_context(file, table_name, error, row_number);
    } catch (...) {
        context.write_state_valid = false;
        throw;
    }
}

[[noreturn]] void unsupported_table_write(const PageFile& file,
                                          const std::string& table_name,
                                          const char* message) {
    throw_table_error(DatabaseErrorCode::unsupported_operation, file, table_name, message);
}

[[noreturn]] void unsupported_column_write(const PageFile& file,
                                           const ReadableTableSchema& table,
                                           const ColumnDefinition& column,
                                           const char* message) {
    DatabaseErrorContext context;
    context.table_name = table.name;
    context.column_number = column.column_number;
    context.column_name = column.name;
    throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                        message,
                        file.path(),
                        table.definition_page_number,
                        std::move(context));
}

std::uint64_t fixed_row_data_size(const PageFile& file,
                                  const ReadableTableSchema& table,
                                  const TableDefinition& definition) {
    if (definition.maximum_column_count == 0 ||
        definition.maximum_column_count != definition.columns.size()) {
        unsupported_table_write(file,
                                table.name,
                                "Row writing requires a dense schema without deleted columns");
    }
    std::uint64_t data_size = 0;
    std::vector<const ColumnDefinition*> variable_columns(definition.variable_column_count, nullptr);
    for (std::size_t index = 0; index < definition.columns.size(); ++index) {
        const auto& column = definition.columns[index];
        if (column.column_number != index) {
            unsupported_column_write(file,
                                     table,
                                     column,
                                     "Row writing requires contiguous column numbers");
        }
        if (jet4_is_boolean_type(column.type)) {
            // BOOLEAN values live in the row NULL mask and occupy no fixed data bytes.
            if ((column.flags & 0x01) == 0 || column.length != 1 || column.fixed_data_offset != 0) {
                unsupported_column_write(file,
                                         table,
                                         column,
                                         "BOOLEAN column has an unsupported Jet4 storage layout");
            }
            continue;
        }
        const auto* fixed_type = jet4_fixed_type(column.type);
        const auto* variable_type = jet4_variable_type(column.type);
        if ((fixed_type == nullptr) == (variable_type == nullptr)) {
            unsupported_column_write(file, table, column, "Row writing encountered an unsupported column type");
        }
        if (fixed_type != nullptr) {
            if ((column.flags & 0x01) == 0 || column.length != fixed_type->byte_size) {
                unsupported_column_write(file,
                                         table,
                                         column,
                                         "Fixed column has an unsupported Jet4 storage layout");
            }
            if (column.fixed_data_offset != data_size) {
                unsupported_column_write(file,
                                         table,
                                         column,
                                         "Row writing requires contiguous fixed-column offsets");
            }
            data_size += column.length;
            continue;
        }

        const bool long_value_type = jet4_is_long_value_type(variable_type->public_type);
        if ((column.flags & 0x01) != 0 || column.variable_table_index >= variable_columns.size() ||
            variable_columns[column.variable_table_index] != nullptr ||
            (long_value_type ? column.length != 0
                             : (column.length == 0 || column.length > variable_type->maximum_byte_size))) {
            unsupported_column_write(file,
                                     table,
                                     column,
                                     "Variable column has an unsupported Jet4 storage layout");
        }
        if (variable_type->public_type == DataType::text && column.length % 2 != 0) {
            unsupported_column_write(file, table, column, "TEXT column has an odd UTF-16 byte capacity");
        }
        variable_columns[column.variable_table_index] = &column;
    }
    if (std::any_of(variable_columns.begin(), variable_columns.end(), [](const auto* column) {
            return column == nullptr;
        })) {
        unsupported_table_write(file, table.name, "Variable-column indexes are not contiguous");
    }
    return data_size;
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

[[noreturn]] void invalid_column_value(const PageFile& file,
                                       const ReadableTableSchema& table,
                                       const ColumnDefinition& column,
                                       const char* message) {
    unsupported_column_write(file, table, column, message);
}

std::int64_t encode_money(const PageFile& file,
                          const ReadableTableSchema& table,
                          const ColumnDefinition& column,
                          std::string_view text) {
    if (text.empty()) {
        invalid_column_value(file, table, column, "MONEY value must be a decimal number");
    }

    std::size_t index = 0;
    bool negative = false;
    if (text[index] == '+' || text[index] == '-') {
        negative = text[index] == '-';
        if (++index == text.size()) {
            invalid_column_value(file, table, column, "MONEY value must be a decimal number");
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
                invalid_column_value(file, table, column, "MONEY value must be a decimal number");
            }
            decimal_separator_seen = true;
            continue;
        }
        if (character < '0' || character > '9') {
            invalid_column_value(file, table, column, "MONEY value must be a decimal number");
        }
        digit_seen = true;
        digits.push_back(character);
        if (decimal_separator_seen) {
            ++fraction_digit_count;
        }
    }
    if (!digit_seen) {
        invalid_column_value(file, table, column, "MONEY value must be a decimal number");
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
            invalid_column_value(file, table, column, "MONEY exponent must contain decimal digits");
        }
        for (; index < text.size(); ++index) {
            const char character = text[index];
            if (character < '0' || character > '9') {
                invalid_column_value(file, table, column, "MONEY exponent must contain decimal digits");
            }
            const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
            if (exponent_magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                invalid_column_value(file, table, column, "MONEY exponent is outside the decimal scale range");
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
            invalid_column_value(file, table, column, "MONEY decimal scale is outside the supported range");
        }
        scale = static_cast<std::int64_t>(fraction_digits + exponent_magnitude);
    } else if (fraction_digits >= exponent_magnitude) {
        const std::uint64_t positive_scale = fraction_digits - exponent_magnitude;
        if (positive_scale > maximum_scale) {
            invalid_column_value(file, table, column, "MONEY decimal scale is outside the supported range");
        }
        scale = static_cast<std::int64_t>(positive_scale);
    } else {
        const std::uint64_t negative_scale = exponent_magnitude - fraction_digits;
        if (negative_scale > minimum_scale_magnitude) {
            invalid_column_value(file, table, column, "MONEY decimal scale is outside the supported range");
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
            invalid_column_value(file, table, column, "MONEY value has more than four non-zero decimal places");
        }
        const std::size_t retained_size = digits.size() - static_cast<std::size_t>(removed_digits);
        if (std::any_of(digits.begin() + static_cast<std::ptrdiff_t>(retained_size), digits.end(), [](char digit) {
                return digit != '0';
            })) {
            invalid_column_value(file, table, column, "MONEY value has more than four non-zero decimal places");
        }
        digits.resize(retained_size);
    } else if (shift > 0) {
        if (shift > 19 || digits.size() + static_cast<std::size_t>(shift) > 19) {
            invalid_column_value(file, table, column, "MONEY value exceeds the Jet4 currency range");
        }
        digits.append(static_cast<std::size_t>(shift), '0');
    }

    constexpr std::uint64_t min_magnitude = std::uint64_t{1} << 63;
    constexpr std::uint64_t max_positive = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const std::uint64_t max_magnitude = negative ? min_magnitude : max_positive;
    const std::string max_magnitude_text = std::to_string(max_magnitude);
    if (digits.size() > max_magnitude_text.size() ||
        (digits.size() == max_magnitude_text.size() && digits > max_magnitude_text)) {
        invalid_column_value(file, table, column, "MONEY value exceeds the Jet4 currency range");
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

std::array<std::uint8_t, 16> encode_guid(const PageFile& file,
                                        const ReadableTableSchema& table,
                                        const ColumnDefinition& column,
                                        std::string_view text) {
    if (text.size() == 38 && text.front() == '{' && text.back() == '}') {
        text.remove_prefix(1);
        text.remove_suffix(1);
    }
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') {
        invalid_column_value(file, table, column, "GUID value must use canonical UUID text with optional braces");
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
            invalid_column_value(file, table, column, "GUID value contains invalid hexadecimal data");
        }
        const int high = hex_digit(text[offset]);
        const int low = hex_digit(text[offset + 1]);
        if (high < 0 || low < 0) {
            invalid_column_value(file, table, column, "GUID value contains invalid hexadecimal data");
        }
        canonical_bytes[byte_index++] = static_cast<std::uint8_t>((high << 4) | low);
        offset += 2;
    }
    if (byte_index != canonical_bytes.size()) {
        invalid_column_value(file, table, column, "GUID value must contain exactly 16 bytes");
    }

    return {canonical_bytes[3], canonical_bytes[2], canonical_bytes[1], canonical_bytes[0],
            canonical_bytes[5], canonical_bytes[4], canonical_bytes[7], canonical_bytes[6],
            canonical_bytes[8], canonical_bytes[9], canonical_bytes[10], canonical_bytes[11],
            canonical_bytes[12], canonical_bytes[13], canonical_bytes[14], canonical_bytes[15]};
}

std::vector<std::uint8_t> encode_utf8_text(const PageFile& file,
                                           const ReadableTableSchema& table,
                                           const ColumnDefinition& column,
                                           const std::string& value,
                                           std::uint64_t maximum_code_units,
                                           const char* size_error) {
    std::vector<std::uint8_t> encoded;
    encoded.reserve(value.size());
    std::size_t offset = 0;
    std::uint64_t code_unit_count = 0;
    while (offset < value.size()) {
        const auto first = static_cast<std::uint8_t>(value[offset]);
        std::uint32_t code_point = 0;
        std::size_t sequence_length = 0;
        if (first <= 0x7f) {
            code_point = first;
            sequence_length = 1;
        } else if (first >= 0xc2 && first <= 0xdf) {
            code_point = first & 0x1f;
            sequence_length = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            code_point = first & 0x0f;
            sequence_length = 3;
        } else if (first >= 0xf0 && first <= 0xf4) {
            code_point = first & 0x07;
            sequence_length = 4;
        } else {
            invalid_column_value(file, table, column, "TEXT value is not valid UTF-8");
        }
        if (sequence_length > value.size() - offset) {
            invalid_column_value(file, table, column, "TEXT value ends in an incomplete UTF-8 sequence");
        }
        for (std::size_t index = 1; index < sequence_length; ++index) {
            const auto continuation = static_cast<std::uint8_t>(value[offset + index]);
            if ((continuation & 0xc0) != 0x80) {
                invalid_column_value(file, table, column, "TEXT value contains an invalid UTF-8 continuation byte");
            }
            code_point = (code_point << 6) | (continuation & 0x3f);
        }
        if ((sequence_length == 2 && code_point < 0x80) ||
            (sequence_length == 3 && code_point < 0x800) ||
            (sequence_length == 4 && code_point < 0x10000) ||
            (code_point >= 0xd800 && code_point <= 0xdfff) || code_point > 0x10ffff) {
            invalid_column_value(file, table, column, "TEXT value contains an invalid UTF-8 code point");
        }

        if (code_point <= 0xffff) {
            encoded.push_back(static_cast<std::uint8_t>(code_point & 0xff));
            encoded.push_back(static_cast<std::uint8_t>((code_point >> 8) & 0xff));
            ++code_unit_count;
        } else {
            const std::uint32_t scalar = code_point - 0x10000;
            const std::uint16_t high = static_cast<std::uint16_t>(0xd800 + (scalar >> 10));
            const std::uint16_t low = static_cast<std::uint16_t>(0xdc00 + (scalar & 0x3ff));
            encoded.push_back(static_cast<std::uint8_t>(high & 0xff));
            encoded.push_back(static_cast<std::uint8_t>((high >> 8) & 0xff));
            encoded.push_back(static_cast<std::uint8_t>(low & 0xff));
            encoded.push_back(static_cast<std::uint8_t>((low >> 8) & 0xff));
            code_unit_count += 2;
        }
        offset += sequence_length;
        if (code_unit_count > maximum_code_units) {
            invalid_column_value(file, table, column, size_error);
        }
    }
    return encoded;
}

std::vector<std::uint8_t> encode_memo_text(const PageFile& file,
                                          const ReadableTableSchema& table,
                                          const ColumnDefinition& column,
                                          const std::string& value) {
    return encode_utf8_text(file,
                            table,
                            column,
                            value,
                            static_cast<std::uint64_t>(long_value_length_mask) / 2,
                            "MEMO value exceeds the Jet4 long-value length limit");
}

TableDefinition read_current_table_definition(const PageFile& file,
                                              const ReadableTableSchema& table) {
    const TableDefinition& cached_definition = table.definition;
    TableDefinition definition = read_table_definition(file, table.definition_page_number);
    if (definition.row_count != cached_definition.row_count) {
        throw_table_error(DatabaseErrorCode::io_error,
                          file,
                          table.name,
                          "Table row count changed outside this writable handle",
                          table.definition_page_number);
    }
    if (definition.columns.size() != cached_definition.columns.size()) {
        throw_table_error(DatabaseErrorCode::io_error,
                          file,
                          table.name,
                          "Table schema changed outside this writable handle",
                          table.definition_page_number);
    }
    const auto same_usage_map_reference = [](const std::optional<UsageMapReference>& left,
                                             const std::optional<UsageMapReference>& right) {
        return left.has_value() == right.has_value() &&
               (!left || (left->row_number == right->row_number && left->page_number == right->page_number));
    };
    for (std::size_t index = 0; index < definition.columns.size(); ++index) {
        auto& column = definition.columns[index];
        const auto& cached_column = cached_definition.columns[index];
        if (column.name != cached_column.name || column.column_number != cached_column.column_number ||
            column.variable_table_index != cached_column.variable_table_index ||
            column.column_id != cached_column.column_id || column.type != cached_column.type ||
            column.precision != cached_column.precision || column.scale != cached_column.scale ||
            column.flags != cached_column.flags || column.extended_flags != cached_column.extended_flags ||
            column.fixed_data_offset != cached_column.fixed_data_offset || column.length != cached_column.length ||
            !same_usage_map_reference(column.long_value_owned_pages, cached_column.long_value_owned_pages) ||
            !same_usage_map_reference(column.long_value_free_space_pages, cached_column.long_value_free_space_pages)) {
            throw_table_error(DatabaseErrorCode::io_error,
                              file,
                              table.name,
                              "Table schema changed outside this writable handle",
                              table.definition_page_number);
        }
        column.nullable = cached_column.nullable;
    }
    return definition;
}

void validate_unindexed_local_table(const PageFile& file,
                                    const ReadableTableSchema& table,
                                    const TableDefinition& definition) {
    if (file.format().version != EngineVersion::jet4 || file.format().page_size != 4096) {
        unsupported_table_write(file, table.name, "Row modification requires a Jet4 4 KB page database");
    }
    if (!table.is_user_table || table.is_linked) {
        unsupported_table_write(file, table.name, "Rows can only be modified in local user tables");
    }
    const TableDefinition system_definition = read_table_definition(file, 2);
    const auto objects = read_system_catalog_objects(file, system_definition, std::string_view{"MSysDb"});
    const auto database_properties = std::find_if(objects.begin(), objects.end(), [](const auto& object) {
        return object.type && *object.type == 2 && object.name && *object.name == "MSysDb";
    });
    if (database_properties == objects.end()) {
        unsupported_table_write(file,
                                table.name,
                                "Row modification requires a database identified as Jet4 V2000");
    }
    if (database_properties->property_error) {
        DatabaseErrorContext context;
        context.table_name = table.name;
        context.row_number = database_properties->row_number;
        throw DatabaseError(DatabaseErrorCode::corrupted_file,
                            *database_properties->property_error,
                            file.path(),
                            database_properties->property_error_page,
                            std::move(context));
    }
    const auto access_version = database_properties->property_bytes
                                    ? read_text_property(file,
                                                         "MSysDb",
                                                         database_properties->page_number,
                                                         database_properties->row_number,
                                                         *database_properties->property_bytes,
                                                         "AccessVersion")
                                    : std::nullopt;
    // V2000 templates carry AccessVersion 08.50 and V2003 templates 09.50; both share the
    // Jet4 row layout, so the writer accepts either marker and rejects everything else.
    if (!access_version || (*access_version != "08.50" && *access_version != "09.50")) {
        unsupported_table_write(file,
                                table.name,
                                "Row modification currently supports Jet4 V2000 and V2003 databases only");
    }
    if (!definition.indexes.empty() || !definition.physical_indexes.empty()) {
        unsupported_table_write(file,
                                table.name,
                                "Row modification currently does not maintain user indexes");
    }
}

struct PendingLongValue {
    std::uint16_t column_number;
    std::size_t definition_offset;
    std::vector<std::uint8_t> bytes;
};

struct EncodedRow {
    std::vector<std::uint8_t> bytes;
    std::vector<PendingLongValue> pending_long_values;
};

EncodedRow encode_row(const PageFile& file,
                      const ReadableTableSchema& table,
                      const TableDefinition& definition,
                      const std::vector<Value>& values,
                      std::uint64_t fixed_data_size) {
    if (values.size() != definition.columns.size()) {
        throw std::invalid_argument("Row value count does not match the table column count");
    }

    const std::uint64_t null_mask_size =
        (static_cast<std::uint64_t>(definition.maximum_column_count) + 7) / 8;
    std::vector<std::uint8_t> row(static_cast<std::size_t>(2 + fixed_data_size), 0);
    write_u16(row, 0, definition.maximum_column_count);
    std::vector<std::uint8_t> null_mask(static_cast<std::size_t>(null_mask_size), 0);
    std::vector<const ColumnDefinition*> variable_columns(definition.variable_column_count, nullptr);
    std::vector<std::optional<std::vector<std::uint8_t>>> variable_values(definition.variable_column_count);
    std::vector<std::optional<std::vector<std::uint8_t>>> long_value_payloads(definition.variable_column_count);

    for (std::size_t index = 0; index < definition.columns.size(); ++index) {
        const auto& column = definition.columns[index];
        const auto* variable_type = jet4_variable_type(column.type);
        if (variable_type != nullptr) {
            variable_columns[column.variable_table_index] = &column;
        }
        const auto& value = values[index];
        if (std::holds_alternative<std::monostate>(value)) {
            if (column.nullable != std::optional<bool>(true)) {
                DatabaseErrorContext context;
                context.table_name = table.name;
                context.column_number = column.column_number;
                context.column_name = column.name;
                throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                                    "Cannot insert NULL into a column that is not known to be nullable",
                                    file.path(),
                                    table.definition_page_number,
                                    std::move(context));
            }
            continue;
        }

        if (jet4_is_boolean_type(column.type)) {
            // A BOOLEAN is stored as a single NULL mask bit: set means TRUE, clear means FALSE.
            // Jet cannot distinguish a NULL boolean from FALSE, so both encode identically.
            const auto* boolean_value = std::get_if<bool>(&value);
            if (boolean_value == nullptr) {
                invalid_column_value(file, table, column, "Row value does not match its BOOLEAN column type");
            }
            if (*boolean_value) {
                null_mask[column.column_number / 8] |=
                    static_cast<std::uint8_t>(1u << (column.column_number % 8));
            }
            continue;
        }

        const auto* fixed_type = jet4_fixed_type(column.type);
        if (fixed_type != nullptr) {
            const std::uint64_t value_offset = 2 + column.fixed_data_offset;
            if (column.fixed_data_offset > fixed_data_size ||
                column.length > fixed_data_size - column.fixed_data_offset) {
                throw_table_error(DatabaseErrorCode::corrupted_file,
                                  file,
                                  table.name,
                                  "Fixed column extends beyond its fixed row data",
                                  table.definition_page_number);
            }

            bool value_encoded = false;
            switch (column.type) {
                case jet4_byte_type_code:
                    if (const auto* byte_value = std::get_if<std::int8_t>(&value)) {
                        row[value_offset] = static_cast<std::uint8_t>(*byte_value);
                        value_encoded = true;
                    }
                    break;
                case jet4_short_type_code:
                    if (const auto* short_value = std::get_if<std::int16_t>(&value)) {
                        write_u16(row, value_offset, static_cast<std::uint16_t>(*short_value));
                        value_encoded = true;
                    }
                    break;
                case jet4_long_type_code:
                    if (const auto* long_value = std::get_if<std::int32_t>(&value)) {
                        write_u32(row, value_offset, static_cast<std::uint32_t>(*long_value));
                        value_encoded = true;
                    }
                    break;
                case jet4_money_type_code:
                    if (const auto* money_value = std::get_if<DecimalValue>(&value)) {
                        const std::int64_t scaled_value = encode_money(file, table, column, money_value->text);
                        write_u64(row, value_offset, static_cast<std::uint64_t>(scaled_value));
                        value_encoded = true;
                    }
                    break;
                case jet4_float_type_code:
                    if (const auto* float_value = std::get_if<float>(&value)) {
                        std::uint32_t bits = 0;
                        std::memcpy(&bits, float_value, sizeof(bits));
                        write_u32(row, value_offset, bits);
                        value_encoded = true;
                    }
                    break;
                case jet4_double_type_code:
                    if (const auto* double_value = std::get_if<double>(&value)) {
                        std::uint64_t bits = 0;
                        std::memcpy(&bits, double_value, sizeof(bits));
                        write_u64(row, value_offset, bits);
                        value_encoded = true;
                    }
                    break;
                case jet4_guid_type_code:
                    if (const auto* guid_value = std::get_if<GuidValue>(&value)) {
                        const auto encoded = encode_guid(file, table, column, guid_value->text);
                        std::copy(encoded.begin(), encoded.end(), row.begin() + value_offset);
                        value_encoded = true;
                    }
                    break;
                default:
                    break;
            }
            if (!value_encoded) {
                invalid_column_value(file,
                                     table,
                                     column,
                                     "Row value does not match its fixed column type");
            }
        } else if (variable_type != nullptr) {
            if (column.variable_table_index >= variable_values.size()) {
                throw_table_error(DatabaseErrorCode::corrupted_file,
                                  file,
                                  table.name,
                                  "Variable column index exceeds its table definition",
                                  table.definition_page_number);
            }
            if (variable_type->public_type == DataType::text) {
                const auto* text_value = std::get_if<std::string>(&value);
                if (text_value == nullptr) {
                    invalid_column_value(file, table, column, "Row value does not match its TEXT column type");
                }
                auto encoded = encode_utf8_text(file,
                                                table,
                                                column,
                                                *text_value,
                                                column.length / 2,
                                                "TEXT value exceeds the column's byte limit");
                if (encoded.size() > column.length) {
                    invalid_column_value(file, table, column, "TEXT value exceeds the column's byte limit");
                }
                variable_values[column.variable_table_index] = std::move(encoded);
            } else if (variable_type->public_type == DataType::binary) {
                const auto* binary_value = std::get_if<BlobValue>(&value);
                if (binary_value == nullptr) {
                    invalid_column_value(file, table, column, "Row value does not match its BINARY column type");
                }
                if (binary_value->bytes.size() > column.length) {
                    invalid_column_value(file, table, column, "BINARY value exceeds the column's byte limit");
                }
                variable_values[column.variable_table_index] = binary_value->bytes;
            } else if (variable_type->public_type == DataType::memo ||
                       variable_type->public_type == DataType::ole) {
                std::vector<std::uint8_t> long_value;
                if (variable_type->public_type == DataType::memo) {
                    const auto* memo_value = std::get_if<MemoValue>(&value);
                    if (memo_value == nullptr) {
                        invalid_column_value(file, table, column, "Row value does not match its MEMO column type");
                    }
                    long_value = memo_value->raw_bytes.empty()
                                     ? encode_memo_text(file, table, column, memo_value->text)
                                     : memo_value->raw_bytes;
                } else {
                    const auto* binary_value = std::get_if<BlobValue>(&value);
                    if (binary_value == nullptr) {
                        invalid_column_value(file, table, column, "Row value does not match its OLE column type");
                    }
                    long_value = binary_value->bytes;
                }
                if (long_value.size() > long_value_length_mask) {
                    invalid_column_value(file, table, column, "Long-value data exceeds the Jet4 length limit");
                }
                std::vector<std::uint8_t> definition_bytes(12, 0);
                if (long_value.empty()) {
                    write_u32(definition_bytes, 0, long_value_inline_flag);
                } else {
                    long_value_payloads[column.variable_table_index] = std::move(long_value);
                }
                variable_values[column.variable_table_index] = std::move(definition_bytes);
            } else {
                invalid_column_value(file, table, column, "Row value uses an unsupported Jet4 variable column type");
            }
        } else {
            invalid_column_value(file, table, column, "Row value uses an unsupported Jet4 column type");
        }
        null_mask[column.column_number / 8] |=
            static_cast<std::uint8_t>(1u << (column.column_number % 8));
    }

    std::vector<std::uint16_t> variable_offsets;
    if (!variable_columns.empty()) {
        variable_offsets.resize(variable_columns.size());
        for (std::size_t index = 0; index < variable_columns.size(); ++index) {
            if (variable_columns[index] == nullptr) {
                throw_table_error(DatabaseErrorCode::corrupted_file,
                                  file,
                                  table.name,
                                  "Variable-column indexes are not contiguous",
                                  table.definition_page_number);
            }
            if (row.size() > std::numeric_limits<std::uint16_t>::max()) {
                unsupported_table_write(file, table.name, "Encoded row exceeds Jet4 variable-column offset limits");
            }
            variable_offsets[index] = static_cast<std::uint16_t>(row.size());
            if (variable_values[index]) {
                row.insert(row.end(), variable_values[index]->begin(), variable_values[index]->end());
            }
        }
        if (row.size() > std::numeric_limits<std::uint16_t>::max()) {
            unsupported_table_write(file, table.name, "Encoded row exceeds Jet4 variable-column offset limits");
        }
        append_u16(row, static_cast<std::uint16_t>(row.size()));
        for (auto offset = variable_offsets.rbegin(); offset != variable_offsets.rend(); ++offset) {
            append_u16(row, *offset);
        }
        append_u16(row, definition.variable_column_count);
    }

    row.insert(row.end(), null_mask.begin(), null_mask.end());
    EncodedRow encoded{std::move(row), {}};
    for (std::size_t index = 0; index < long_value_payloads.size(); ++index) {
        if (!long_value_payloads[index]) {
            continue;
        }
        encoded.pending_long_values.push_back({variable_columns[index]->column_number,
                                               variable_offsets[index],
                                               std::move(*long_value_payloads[index])});
    }
    return encoded;
}

bool row_fits_on_data_page(const PageFile& file,
                           const std::string& table_name,
                           const std::vector<std::uint8_t>& page,
                           std::uint64_t row_size) {
    const std::uint16_t row_count = read_u16(page, data_page_row_count_offset);
    const std::uint16_t free_space = read_u16(page, 2);
    if (row_count > 255 || free_space > page.size() - data_page_row_directory_offset) {
        throw_table_error(DatabaseErrorCode::corrupted_file,
                          file,
                          table_name,
                          "Data page row count or free space is invalid");
    }
    if (row_count == 255 || row_size + 2 > free_space) {
        return false;
    }

    const std::uint64_t directory_end =
        data_page_row_directory_offset + static_cast<std::uint64_t>(row_count + 1) * 2;
    const std::uint16_t previous_start = row_count == 0
                                             ? static_cast<std::uint16_t>(page.size())
                                             : static_cast<std::uint16_t>(
                                                   read_u16(page,
                                                            data_page_row_directory_offset +
                                                                static_cast<std::uint64_t>(row_count - 1) * 2) &
                                                   data_page_row_offset_mask);
    return row_size <= previous_start && previous_start - row_size >= directory_end;
}

DataPage validate_data_page(const PageFile& file,
                            const std::string& table_name,
                            std::uint64_t page_number,
                            std::uint64_t definition_page_number,
                            const std::vector<std::uint8_t>& page) {
    DataPage parsed = parse_data_page(file, page_number, page);
    if (read_u32(page, data_page_table_definition_offset) != definition_page_number) {
        throw_table_error(DatabaseErrorCode::corrupted_file,
                          file,
                          table_name,
                          "Data page references a different table definition",
                          page_number);
    }
    return parsed;
}

std::vector<std::uint8_t> new_data_page(std::uint32_t page_size,
                                        std::uint64_t definition_page_number) {
    std::vector<std::uint8_t> page(page_size, 0);
    page[0] = static_cast<std::uint8_t>(PageType::data);
    page[1] = 0x01;
    write_u16(page, 2, static_cast<std::uint16_t>(page_size - data_page_row_directory_offset));
    write_u32(page, data_page_table_definition_offset, static_cast<std::uint32_t>(definition_page_number));
    write_u16(page, data_page_row_count_offset, 0);
    return page;
}

std::uint16_t append_row_to_page(std::vector<std::uint8_t>& page,
                                const std::vector<std::uint8_t>& row,
                                std::uint16_t row_flags = 0) {
    const std::uint16_t row_count = read_u16(page, data_page_row_count_offset);
    const std::uint16_t previous_start = row_count == 0
                                             ? static_cast<std::uint16_t>(page.size())
                                             : static_cast<std::uint16_t>(
                                                   read_u16(page,
                                                            data_page_row_directory_offset +
                                                                static_cast<std::uint64_t>(row_count - 1) * 2) &
                                                   data_page_row_offset_mask);
    const std::uint16_t start = static_cast<std::uint16_t>(previous_start - row.size());
    std::copy(row.begin(), row.end(), page.begin() + start);
    write_u16(page,
              data_page_row_directory_offset + static_cast<std::uint64_t>(row_count) * 2,
              static_cast<std::uint16_t>(start | row_flags));
    write_u16(page, data_page_row_count_offset, static_cast<std::uint16_t>(row_count + 1));
    const std::uint16_t free_space = read_u16(page, 2);
    write_u16(page, 2, static_cast<std::uint16_t>(free_space - row.size() - 2));
    return row_count;
}

std::vector<std::uint8_t> new_long_value_page(std::uint32_t page_size) {
    std::vector<std::uint8_t> page(page_size, 0);
    page[0] = static_cast<std::uint8_t>(PageType::data);
    page[1] = 0x01;
    write_u16(page, 2, static_cast<std::uint16_t>(page_size - data_page_row_directory_offset));
    page[4] = 'L';
    page[5] = 'V';
    page[6] = 'A';
    page[7] = 'L';
    write_u16(page, data_page_row_count_offset, 0);
    return page;
}

struct LongValuePageSet {
    std::map<std::uint64_t, std::vector<std::uint8_t>> pages;
    std::map<std::uint16_t, std::set<std::uint64_t>> pages_by_column;
    std::uint64_t next_page_number;
};

LongValuePageSet allocate_long_value_pages(const PageFile& file,
                                           const std::string& table_name,
                                           EncodedRow& row,
                                           std::uint64_t first_page_number) {
    LongValuePageSet result{{}, {}, first_page_number};
    for (auto& pending : row.pending_long_values) {
        if (pending.bytes.empty() || pending.bytes.size() > long_value_length_mask) {
            throw_table_error(DatabaseErrorCode::corrupted_file,
                              file,
                              table_name,
                              "Pending long-value data has an invalid length");
        }
        const std::size_t chunk_capacity =
            pending.bytes.size() <= jet4_v2000_max_long_value_row_size
                ? pending.bytes.size()
                : jet4_v2000_max_long_value_row_size - long_value_page_pointer_size;
        const std::size_t page_count = pending.bytes.size() <= jet4_v2000_max_long_value_row_size
                                           ? 1
                                           : (pending.bytes.size() + chunk_capacity - 1) / chunk_capacity;
        if (result.next_page_number >= 0x1000000 ||
            page_count > 0x1000000 - result.next_page_number) {
            unsupported_table_write(file, table_name, "Jet4 long-value page exceeds the three-byte page reference limit");
        }
        const std::uint64_t first_page = result.next_page_number;
        const bool chained = pending.bytes.size() > jet4_v2000_max_long_value_row_size;
        std::size_t data_offset = 0;
        for (std::size_t page_index = 0; page_index < page_count; ++page_index) {
            const std::uint64_t page_number = first_page + page_index;
            std::vector<std::uint8_t> page = new_long_value_page(file.format().page_size);
            const std::size_t remaining = pending.bytes.size() - data_offset;
            const std::size_t data_size = std::min(chunk_capacity, remaining);
            std::vector<std::uint8_t> row_bytes;
            if (chained) {
                row_bytes.resize(static_cast<std::size_t>(long_value_page_pointer_size), 0);
                if (page_index + 1 < page_count) {
                    row_bytes[0] = 0;
                    write_u24_le(row_bytes, 1, static_cast<std::uint32_t>(page_number + 1));
                }
            }
            row_bytes.insert(row_bytes.end(),
                             pending.bytes.begin() + static_cast<std::ptrdiff_t>(data_offset),
                             pending.bytes.begin() + static_cast<std::ptrdiff_t>(data_offset + data_size));
            if (!row_fits_on_data_page(file, table_name, page, row_bytes.size())) {
                throw_table_error(DatabaseErrorCode::corrupted_file,
                                  file,
                                  table_name,
                                  "Long-value row does not fit on a Jet4 V2000 page",
                                  page_number);
            }
            if (append_row_to_page(page, row_bytes) != 0) {
                throw_table_error(DatabaseErrorCode::corrupted_file,
                                  file,
                                  table_name,
                                  "New long-value page did not start with row zero",
                                  page_number);
            }
            result.pages.emplace(page_number, std::move(page));
            result.pages_by_column[pending.column_number].insert(page_number);
            data_offset += data_size;
        }

        if (data_offset != pending.bytes.size()) {
            throw_table_error(DatabaseErrorCode::corrupted_file,
                              file,
                              table_name,
                              "Long-value pages did not contain the declared value length",
                              first_page);
        }
        if (pending.definition_offset > row.bytes.size() ||
            12 > row.bytes.size() - pending.definition_offset) {
            throw_table_error(DatabaseErrorCode::corrupted_file,
                              file,
                              table_name,
                              "Long-value definition extends beyond its encoded row");
        }
        const std::uint32_t type_flag = chained ? 0 : long_value_other_page_flag;
        write_u32(row.bytes,
                  pending.definition_offset,
                  static_cast<std::uint32_t>(pending.bytes.size()) | type_flag);
        row.bytes[pending.definition_offset + 4] = 0;
        write_u24_le(row.bytes, pending.definition_offset + 5, static_cast<std::uint32_t>(first_page));
        write_u32(row.bytes, pending.definition_offset + 8, 0);
        result.next_page_number += page_count;
    }
    return result;
}

LongValuePageSet allocate_long_value_pages(const PageFile& file,
                                           const std::string& table_name,
                                           std::vector<EncodedRow>& rows,
                                           std::uint64_t first_page_number) {
    LongValuePageSet result{{}, {}, first_page_number};
    for (auto& row : rows) {
        LongValuePageSet row_pages = allocate_long_value_pages(file,
                                                               table_name,
                                                               row,
                                                               result.next_page_number);
        result.pages.insert(std::make_move_iterator(row_pages.pages.begin()),
                            std::make_move_iterator(row_pages.pages.end()));
        for (const auto& entry : row_pages.pages_by_column) {
            result.pages_by_column[entry.first].insert(entry.second.begin(), entry.second.end());
        }
        result.next_page_number = row_pages.next_page_number;
    }
    return result;
}

std::vector<std::uint8_t> compact_data_page_with_row_replacement(
    const PageFile& file,
    const std::string& table_name,
    const std::vector<std::uint8_t>& page,
    const DataPage& data_page,
    std::uint16_t row_number,
    const std::vector<std::uint8_t>& replacement_row) {
    if (row_number >= data_page.rows.size()) {
        throw_table_error(DatabaseErrorCode::corrupted_file,
                          file,
                          table_name,
                          "Data-page row to compact is outside its row directory",
                          data_page.page_number);
    }

    const std::uint64_t directory_end =
        data_page_row_directory_offset + static_cast<std::uint64_t>(data_page.rows.size()) * 2;
    if (directory_end > page.size()) {
        throw_table_error(DatabaseErrorCode::corrupted_file,
                          file,
                          table_name,
                          "Data-page row directory exceeds the page while compacting",
                          data_page.page_number);
    }

    std::vector<std::uint8_t> compacted_page = page;
    std::fill(compacted_page.begin() + static_cast<std::ptrdiff_t>(directory_end), compacted_page.end(), 0);

    std::uint16_t next_start = static_cast<std::uint16_t>(compacted_page.size());
    for (std::uint16_t index = 0; index < data_page.rows.size(); ++index) {
        const auto& row = data_page.rows[index];
        const auto& row_bytes = index == row_number ? replacement_row : row.bytes;
        if (next_start < directory_end || row_bytes.size() > next_start - directory_end) {
            throw_table_error(DatabaseErrorCode::corrupted_file,
                              file,
                              table_name,
                              "Compacted data-page rows exceed the available row region",
                              data_page.page_number);
        }

        const auto start = static_cast<std::uint16_t>(next_start - row_bytes.size());
        std::copy(row_bytes.begin(),
                  row_bytes.end(),
                  compacted_page.begin() + static_cast<std::ptrdiff_t>(start));

        const std::uint64_t row_location_offset =
            data_page_row_directory_offset + static_cast<std::uint64_t>(index) * 2;
        const std::uint16_t old_location = read_u16(page, row_location_offset);
        const std::uint16_t row_flags = static_cast<std::uint16_t>(old_location & ~data_page_row_offset_mask);
        write_u16(compacted_page, row_location_offset, static_cast<std::uint16_t>(start | row_flags));
        next_start = start;
    }

    write_u16(compacted_page, 2, static_cast<std::uint16_t>(next_start - directory_end));
    (void)validate_data_page(file,
                             table_name,
                             data_page.page_number,
                             read_u32(page, data_page_table_definition_offset),
                             compacted_page);
    return compacted_page;
}

void update_inline_usage_map_row(const PageFile& file,
                                 const std::string& table_name,
                                 std::vector<std::uint8_t>& usage_page,
                                 const UsageMapReference& reference,
                                 const std::set<std::uint64_t>& add_pages,
                                 const std::set<std::uint64_t>& remove_pages,
                                 bool convert_empty_reference_map) {
    const DataPage parsed = parse_data_page(file, reference.page_number, usage_page);
    if (reference.row_number >= parsed.rows.size()) {
        throw_table_error(DatabaseErrorCode::corrupted_file,
                          file,
                          table_name,
                          "Table usage map references a missing declaration row",
                          reference.page_number);
    }
    const auto& declaration = parsed.rows[reference.row_number];
    if (declaration.deleted || declaration.overflow || declaration.bytes.size() < 5) {
        throw_table_error(DatabaseErrorCode::corrupted_file,
                          file,
                          table_name,
                          "Table usage map declaration is invalid",
                          reference.page_number);
    }

    std::vector<std::uint8_t> bytes = declaration.bytes;
    if (bytes[0] == 1) {
        if (!convert_empty_reference_map ||
            std::any_of(bytes.begin() + 1, bytes.end(), [](std::uint8_t value) { return value != 0; })) {
            unsupported_table_write(file,
                                    table_name,
                                    "Writing to reference usage maps with allocated map pages is not yet supported");
        }
        std::fill(bytes.begin(), bytes.end(), 0);
        bytes[0] = 0;
        write_u32(bytes, 1, 0);
    } else if (bytes[0] != 0) {
        unsupported_table_write(file, table_name, "Unsupported table usage map type for row insertion");
    }

    const std::uint64_t bitmap_size = bytes.size() - 5;
    std::uint64_t first_page = read_u32(bytes, 1);
    std::set<std::uint64_t> pages;
    for (std::uint64_t byte_index = 0; byte_index < bitmap_size; ++byte_index) {
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            if ((bytes[5 + byte_index] & static_cast<std::uint8_t>(1u << bit)) != 0) {
                pages.insert(first_page + byte_index * 8 + bit);
            }
        }
    }
    for (const std::uint64_t page_number : remove_pages) {
        pages.erase(page_number);
    }
    pages.insert(add_pages.begin(), add_pages.end());

    if (!pages.empty()) {
        const std::uint64_t new_first_page = (*pages.begin() / 8) * 8;
        const std::uint64_t last_page = *pages.rbegin();
        if (last_page - new_first_page >= bitmap_size * 8) {
            unsupported_table_write(file, table_name, "Table usage map expansion is not yet supported");
        }
        first_page = new_first_page;
    }
    write_u32(bytes, 1, static_cast<std::uint32_t>(first_page));
    std::fill(bytes.begin() + 5, bytes.end(), 0);
    for (const std::uint64_t page_number : pages) {
        const std::uint64_t bit_index = page_number - first_page;
        if (bit_index >= bitmap_size * 8) {
            unsupported_table_write(file, table_name, "Table usage map expansion is not yet supported");
        }
        bytes[5 + bit_index / 8] |= static_cast<std::uint8_t>(1u << (bit_index % 8));
    }
    std::copy(bytes.begin(), bytes.end(), usage_page.begin() + declaration.start_offset);
}

std::vector<std::uint8_t>& mutable_page(std::map<std::uint64_t, std::vector<std::uint8_t>>& pages,
                                        const PageFile& file,
                                        std::uint64_t page_number) {
    const auto existing = pages.find(page_number);
    if (existing != pages.end()) {
        return existing->second;
    }
    return pages.emplace(page_number, file.read_page(page_number)).first->second;
}

void update_long_value_usage_maps(const PageFile& file,
                                  const std::string& table_name,
                                  const TableDefinition& definition,
                                  const std::map<std::uint16_t, std::set<std::uint64_t>>& pages_by_column,
                                  std::map<std::uint64_t, std::vector<std::uint8_t>>& updated_pages) {
    for (const auto& entry : pages_by_column) {
        const auto column = std::find_if(definition.columns.begin(), definition.columns.end(), [&](const auto& candidate) {
            return candidate.column_number == entry.first;
        });
        if (column == definition.columns.end() || !jet4_is_long_value_type(column->type)) {
            throw_table_error(DatabaseErrorCode::corrupted_file,
                              file,
                              table_name,
                              "Long-value allocation references a non-long-value column");
        }
        if (!column->long_value_owned_pages || !column->long_value_free_space_pages ||
            column->long_value_owned_pages->page_number == 0 ||
            column->long_value_free_space_pages->page_number == 0) {
            DatabaseErrorContext context;
            context.table_name = table_name;
            context.column_number = column->column_number;
            context.column_name = column->name;
            throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                                "Long-value column is missing its Jet4 usage-map references",
                                file.path(),
                                std::nullopt,
                                std::move(context));
        }
        auto& owned_page = mutable_page(updated_pages, file, column->long_value_owned_pages->page_number);
        update_inline_usage_map_row(file,
                                    table_name,
                                    owned_page,
                                    *column->long_value_owned_pages,
                                    entry.second,
                                    {},
                                    false);
        auto& free_page = mutable_page(updated_pages, file, column->long_value_free_space_pages->page_number);
        update_inline_usage_map_row(file,
                                    table_name,
                                    free_page,
                                    *column->long_value_free_space_pages,
                                    entry.second,
                                    {},
                                    false);
    }
}

struct PhysicalRowLocation {
    std::uint64_t page_number;
    std::uint16_t row_number;
    DataPageRow row;
};

PhysicalRowLocation locate_final_row(const PageFile& file,
                                     const ReadableTableSchema& table,
                                     const TableDefinition& definition,
                                     const std::set<std::uint64_t>& owned_pages,
                                     RowId row_id) {
    std::uint64_t page_number = row_id.page_number;
    std::uint16_t row_number = row_id.row_number;
    bool header_row = true;
    std::set<std::pair<std::uint64_t, std::uint16_t>> visited;

    while (true) {
        const RowId current_row_id{page_number, row_number};
        if (!visited.emplace(page_number, row_number).second) {
            throw_row_error(DatabaseErrorCode::corrupted_file,
                            file,
                            table.name,
                            current_row_id,
                            "Row overflow chain contains a cycle");
        }
        if (page_number == 0 || page_number >= file.page_count() ||
            owned_pages.find(page_number) == owned_pages.end()) {
            throw_row_error(DatabaseErrorCode::corrupted_file,
                            file,
                            table.name,
                            current_row_id,
                            "Row overflow pointer references a page outside the table");
        }

        const auto page = file.read_page(page_number);
        const DataPage data_page = validate_data_page(file,
                                                     table.name,
                                                     page_number,
                                                     table.definition_page_number,
                                                     page);
        if (row_number >= data_page.rows.size()) {
            throw_row_error(DatabaseErrorCode::corrupted_file,
                            file,
                            table.name,
                            current_row_id,
                            "Row overflow pointer references a missing data-page row");
        }
        const DataPageRow row = data_page.rows[row_number];
        if (header_row && row.deleted) {
            throw_row_error(DatabaseErrorCode::unsupported_operation,
                            file,
                            table.name,
                            current_row_id,
                            "RowId refers to a deleted row");
        }
        if (!header_row && !row.deleted) {
            throw_row_error(DatabaseErrorCode::corrupted_file,
                            file,
                            table.name,
                            current_row_id,
                            "Row overflow data is not marked deleted");
        }
        if (row.deleted && row.overflow) {
            throw_row_error(DatabaseErrorCode::corrupted_file,
                            file,
                            table.name,
                            current_row_id,
                            "Row overflow pointer is marked both deleted and overflow");
        }
        if (!row.overflow) {
            return {page_number, row_number, row};
        }
        if (row.bytes.size() < 4) {
            throw_row_error(DatabaseErrorCode::corrupted_file,
                            file,
                            table.name,
                            current_row_id,
                            "Row overflow pointer is truncated");
        }

        row_number = row.bytes[0];
        page_number = read_u24_le(row.bytes, 1);
        header_row = false;
    }
}

}

std::vector<RowId> append_jet4_rows(Jet4WriteContext& context,
                                    std::size_t table_index,
                                    const std::vector<std::vector<Value>>& rows) {
    if (rows.empty()) {
        return {};
    }
    if (!context.writable) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Database was opened read-only; reopen it in read-write mode to insert rows",
                            context.file.path());
    }
    std::lock_guard<std::mutex> write_lock(context.write_mutex);
    if (!context.write_state_valid) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Database write state is invalid after a previous write failure; reopen and inspect the file",
                            context.file.path());
    }
    if (table_index >= context.schema.tables.size()) {
        throw std::out_of_range("Table schema index is outside the database catalog");
    }
    const auto& table = context.schema.tables[table_index];
    try {
        const std::filesystem::path path = context.file.path();
        const PageFile file = PageFile::open(path);
        if (file.page_count() != context.file.page_count()) {
            throw_table_error(DatabaseErrorCode::io_error,
                              file,
                              table.name,
                              "Database file size changed outside this writable handle");
        }
        TableDefinition definition = read_current_table_definition(file, table);
        validate_unindexed_local_table(file, table, definition);
        if (definition.owned_pages.page_number == 0 || definition.free_space_pages.page_number == 0 ||
            definition.owned_pages.page_number != definition.free_space_pages.page_number) {
            unsupported_table_write(file,
                                    table.name,
                                    "Row insertion currently requires the table usage maps to share one declaration page");
        }
        if (rows.size() > std::numeric_limits<std::uint32_t>::max() - definition.row_count) {
            unsupported_table_write(file, table.name, "Table row count would exceed Jet4 limits");
        }

        const std::uint64_t fixed_data_size = fixed_row_data_size(file, table, definition);
        std::vector<EncodedRow> encoded_rows;
        encoded_rows.reserve(rows.size());
        for (const auto& row : rows) {
            encoded_rows.push_back(encode_row(file, table, definition, row, fixed_data_size));
        }
        const std::uint64_t maximum_row_size =
            file.format().page_size - data_page_row_directory_offset - 2;
        for (const auto& encoded_row : encoded_rows) {
            if (encoded_row.bytes.size() > maximum_row_size) {
                unsupported_table_write(file, table.name, "Encoded row does not fit on a Jet4 data page");
            }
        }
        LongValuePageSet long_value_pages = allocate_long_value_pages(file,
                                                                      table.name,
                                                                      encoded_rows,
                                                                      file.page_count());
        const auto largest_row = std::max_element(encoded_rows.begin(), encoded_rows.end(), [](const auto& left,
                                                                                              const auto& right) {
            return left.bytes.size() < right.bytes.size();
        });
        const std::size_t largest_row_size = largest_row->bytes.size();

        const auto owned_page_list = read_usage_map_pages(file, definition.owned_pages);
        const auto free_page_list = read_usage_map_pages(file, definition.free_space_pages);
        const std::set<std::uint64_t> owned_pages(owned_page_list.begin(), owned_page_list.end());
        std::set<std::uint64_t> free_pages(free_page_list.begin(), free_page_list.end());
        std::set<std::uint64_t> removed_free_pages;
        std::map<std::uint64_t, std::vector<std::uint8_t>> data_pages;
        std::vector<std::uint64_t> appended_page_numbers;
        for (const auto& page : long_value_pages.pages) {
            data_pages.emplace(page.first, page.second);
            appended_page_numbers.push_back(page.first);
        }
        std::vector<std::uint64_t> appended_data_page_numbers;
        std::uint64_t next_page_number = long_value_pages.next_page_number;
        std::optional<std::uint64_t> active_page_number;

        for (auto iterator = free_page_list.rbegin(); iterator != free_page_list.rend(); ++iterator) {
            const std::uint64_t page_number = *iterator;
            if (owned_pages.find(page_number) == owned_pages.end()) {
                throw_table_error(DatabaseErrorCode::corrupted_file,
                                  file,
                                  table.name,
                                  "Free-space map references a page not owned by the table",
                                  page_number);
            }
            auto page = file.read_page(page_number);
            validate_data_page(file, table.name, page_number, table.definition_page_number, page);
            if (!row_fits_on_data_page(file, table.name, page, largest_row_size)) {
                free_pages.erase(page_number);
                removed_free_pages.insert(page_number);
                continue;
            }
            data_pages.emplace(page_number, std::move(page));
            active_page_number = page_number;
            break;
        }

        std::vector<RowId> row_ids;
        row_ids.reserve(encoded_rows.size());
        for (const auto& row : encoded_rows) {
            if (!active_page_number ||
                !row_fits_on_data_page(file, table.name, data_pages.at(*active_page_number), row.bytes.size())) {
                if (active_page_number) {
                    free_pages.erase(*active_page_number);
                    removed_free_pages.insert(*active_page_number);
                }
                if (next_page_number >= 0x1000000) {
                    unsupported_table_write(file, table.name, "Jet4 data page exceeds the three-byte page reference limit");
                }
                const std::uint64_t page_number = next_page_number++;
                data_pages.emplace(page_number,
                                   new_data_page(file.format().page_size, table.definition_page_number));
                appended_page_numbers.push_back(page_number);
                appended_data_page_numbers.push_back(page_number);
                active_page_number = page_number;
            }

            auto& page = data_pages.at(*active_page_number);
            const std::uint16_t row_number = append_row_to_page(page, row.bytes);
            row_ids.push_back({*active_page_number, row_number});
            if (row_fits_on_data_page(file, table.name, page, row.bytes.size())) {
                free_pages.insert(*active_page_number);
                removed_free_pages.erase(*active_page_number);
            } else {
                free_pages.erase(*active_page_number);
                removed_free_pages.insert(*active_page_number);
            }
        }

        std::vector<std::uint64_t> newly_free_pages;
        for (const std::uint64_t page_number : appended_data_page_numbers) {
            if (free_pages.find(page_number) != free_pages.end()) {
                newly_free_pages.push_back(page_number);
            }
        }
        std::set<std::uint64_t> new_owned_pages(appended_data_page_numbers.begin(),
                                                appended_data_page_numbers.end());
        std::set<std::uint64_t> add_free_pages(newly_free_pages.begin(), newly_free_pages.end());

        auto& updated_usage_page = mutable_page(data_pages, file, definition.owned_pages.page_number);
        update_inline_usage_map_row(file,
                                    table.name,
                                    updated_usage_page,
                                    definition.owned_pages,
                                    new_owned_pages,
                                    {},
                                    true);
        update_inline_usage_map_row(file,
                                    table.name,
                                    updated_usage_page,
                                    definition.free_space_pages,
                                    add_free_pages,
                                    removed_free_pages,
                                    false);
        update_long_value_usage_maps(file,
                                     table.name,
                                     definition,
                                     long_value_pages.pages_by_column,
                                     data_pages);

        auto updated_definition_page = file.read_page(table.definition_page_number);
        write_u32(updated_definition_page,
                  16,
                  definition.row_count + static_cast<std::uint32_t>(rows.size()));
        std::vector<std::uint8_t> updated_global_usage_page;
        if (!appended_page_numbers.empty()) {
            updated_global_usage_page = mark_global_pages_allocated(file, appended_page_numbers);
        }

        write_pages_and_refresh(context,
                                file,
                                table.name,
                                std::nullopt,
                                std::nullopt,
                                [&](PageFileWriter& writer) {
                                    for (const std::uint64_t page_number : appended_page_numbers) {
                                        if (writer.append_page(data_pages.at(page_number)) != page_number) {
                                            throw_table_error(DatabaseErrorCode::io_error,
                                                              file,
                                                              table.name,
                                                              "Row data page was appended at an unexpected page number",
                                                              page_number);
                                        }
                                    }
                                    for (const auto& entry : data_pages) {
                                        if (entry.first < file.page_count()) {
                                            writer.write_page(entry.first, entry.second);
                                        }
                                    }
                                    if (!updated_global_usage_page.empty()) {
                                        writer.write_page(1, updated_global_usage_page);
                                    }
                                    writer.write_page(table.definition_page_number, updated_definition_page);
                                });
        context.schema.tables[table_index].definition.row_count =
            definition.row_count + static_cast<std::uint32_t>(rows.size());
        return row_ids;
    } catch (const DatabaseError& error) {
        rethrow_with_table_context(context.file, table.name, error);
    }
}

void update_jet4_row(Jet4WriteContext& context,
                     std::size_t table_index,
                     RowId row_id,
                     const std::vector<Value>& values) {
    if (!context.writable) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Database was opened read-only; reopen it in read-write mode to update rows",
                            context.file.path());
    }
    std::lock_guard<std::mutex> write_lock(context.write_mutex);
    if (!context.write_state_valid) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Database write state is invalid after a previous write failure; reopen and inspect the file",
                            context.file.path());
    }
    if (table_index >= context.schema.tables.size()) {
        throw std::out_of_range("Table schema index is outside the database catalog");
    }
    const auto& table = context.schema.tables[table_index];
    try {
        const std::filesystem::path path = context.file.path();
        const PageFile file = PageFile::open(path);
        if (file.page_count() != context.file.page_count()) {
            throw_table_error(DatabaseErrorCode::io_error,
                              file,
                              table.name,
                              "Database file size changed outside this writable handle");
        }
        const TableDefinition definition = read_current_table_definition(file, table);
        validate_unindexed_local_table(file, table, definition);
        const std::uint64_t fixed_data_size = fixed_row_data_size(file, table, definition);
        EncodedRow encoded_row = encode_row(file, table, definition, values, fixed_data_size);

        if (row_id.page_number >= file.page_count()) {
            throw_row_error(DatabaseErrorCode::page_out_of_range,
                            file,
                            table.name,
                            row_id,
                            "RowId page is outside the database");
        }
        const auto owned_page_list = read_usage_map_pages(file, definition.owned_pages);
        const std::set<std::uint64_t> owned_pages(owned_page_list.begin(), owned_page_list.end());
        if (owned_pages.find(row_id.page_number) == owned_pages.end()) {
            throw_row_error(DatabaseErrorCode::unsupported_operation,
                            file,
                            table.name,
                            row_id,
                            "RowId page is not owned by this table");
        }

        const PhysicalRowLocation final_row = locate_final_row(file, table, definition, owned_pages, row_id);
        TableRowReader row_reader(file, table.name, definition);
        (void)row_reader.read_at(row_id.page_number, row_id.row_number);
        const std::uint64_t maximum_row_size =
            file.format().page_size - data_page_row_directory_offset - 2;
        if (encoded_row.bytes.size() > maximum_row_size) {
            throw_row_error(DatabaseErrorCode::unsupported_operation,
                            file,
                            table.name,
                            row_id,
                            "Encoded row does not fit on a Jet4 data page");
        }
        LongValuePageSet long_value_pages = allocate_long_value_pages(file,
                                                                      table.name,
                                                                      encoded_row,
                                                                      file.page_count());
        if (encoded_row.bytes.size() == final_row.row.bytes.size()) {
            auto final_page = file.read_page(final_row.page_number);
            const DataPage final_data_page = validate_data_page(file,
                                                               table.name,
                                                               final_row.page_number,
                                                               table.definition_page_number,
                                                               final_page);
            if (final_row.row_number >= final_data_page.rows.size() ||
                final_data_page.rows[final_row.row_number].bytes.size() != encoded_row.bytes.size()) {
                throw_row_error(DatabaseErrorCode::corrupted_file,
                                file,
                                table.name,
                                row_id,
                                "Resolved row data changed during update validation");
            }
            const auto& current_row = final_data_page.rows[final_row.row_number];
            std::copy(encoded_row.bytes.begin(),
                      encoded_row.bytes.end(),
                      final_page.begin() + static_cast<std::ptrdiff_t>(current_row.start_offset));

            std::map<std::uint64_t, std::vector<std::uint8_t>> updated_pages;
            updated_pages.emplace(final_row.page_number, std::move(final_page));
            update_long_value_usage_maps(file,
                                         table.name,
                                         definition,
                                         long_value_pages.pages_by_column,
                                         updated_pages);
            std::vector<std::uint64_t> appended_page_numbers;
            for (const auto& page : long_value_pages.pages) {
                appended_page_numbers.push_back(page.first);
            }
            if (!appended_page_numbers.empty()) {
                updated_pages[1] = mark_global_pages_allocated(file, appended_page_numbers);
            }

            write_pages_and_refresh(context,
                                    file,
                                    table.name,
                                    row_id.row_number,
                                    row_id.page_number,
                                    [&](PageFileWriter& writer) {
                                        for (const auto& page : long_value_pages.pages) {
                                            if (writer.append_page(page.second) != page.first) {
                                                throw_table_error(DatabaseErrorCode::io_error,
                                                                  file,
                                                                  table.name,
                                                                  "Long-value page was appended at an unexpected page number",
                                                                  page.first);
                                            }
                                        }
                                        for (const auto& page : updated_pages) {
                                            if (page.first < file.page_count()) {
                                                writer.write_page(page.first, page.second);
                                            }
                                        }
                                    });
            return;
        }

        if (encoded_row.bytes.size() < final_row.row.bytes.size()) {
            auto final_page = file.read_page(final_row.page_number);
            const DataPage final_data_page = validate_data_page(file,
                                                               table.name,
                                                               final_row.page_number,
                                                               table.definition_page_number,
                                                               final_page);
            if (final_row.row_number >= final_data_page.rows.size() ||
                final_data_page.rows[final_row.row_number].bytes.size() != final_row.row.bytes.size()) {
                throw_row_error(DatabaseErrorCode::corrupted_file,
                                file,
                                table.name,
                                row_id,
                                "Resolved row data changed during shrink validation");
            }

            std::map<std::uint64_t, std::vector<std::uint8_t>> updated_pages;
            updated_pages.emplace(final_row.page_number,
                                  compact_data_page_with_row_replacement(file,
                                                                        table.name,
                                                                        final_page,
                                                                        final_data_page,
                                                                        final_row.row_number,
                                                                        encoded_row.bytes));

            const auto free_page_list = read_usage_map_pages(file, definition.free_space_pages);
            const std::set<std::uint64_t> free_pages(free_page_list.begin(), free_page_list.end());
            if (free_pages.find(final_row.page_number) == free_pages.end() &&
                row_fits_on_data_page(file,
                                      table.name,
                                      updated_pages.at(final_row.page_number),
                                      encoded_row.bytes.size())) {
                const std::set<std::uint64_t> newly_free_pages{final_row.page_number};
                auto usage_page = updated_pages.find(definition.free_space_pages.page_number);
                if (usage_page == updated_pages.end()) {
                    updated_pages.emplace(definition.free_space_pages.page_number,
                                          file.read_page(definition.free_space_pages.page_number));
                }
                update_inline_usage_map_row(file,
                                            table.name,
                                            updated_pages.at(definition.free_space_pages.page_number),
                                            definition.free_space_pages,
                                            newly_free_pages,
                                            {},
                                            true);
            }
            update_long_value_usage_maps(file,
                                         table.name,
                                         definition,
                                         long_value_pages.pages_by_column,
                                         updated_pages);
            std::vector<std::uint64_t> appended_page_numbers;
            for (const auto& page : long_value_pages.pages) {
                appended_page_numbers.push_back(page.first);
            }
            if (!appended_page_numbers.empty()) {
                updated_pages[1] = mark_global_pages_allocated(file, appended_page_numbers);
            }

            write_pages_and_refresh(context,
                                    file,
                                    table.name,
                                    row_id.row_number,
                                    row_id.page_number,
                                    [&](PageFileWriter& writer) {
                                        for (const auto& page : long_value_pages.pages) {
                                            if (writer.append_page(page.second) != page.first) {
                                                throw_table_error(DatabaseErrorCode::io_error,
                                                                  file,
                                                                  table.name,
                                                                  "Long-value page was appended at an unexpected page number",
                                                                  page.first);
                                            }
                                        }
                                        for (const auto& entry : updated_pages) {
                                            if (entry.first < file.page_count()) {
                                                writer.write_page(entry.first, entry.second);
                                            }
                                        }
                                    });
            return;
        }

        if (definition.owned_pages.page_number == 0 || definition.free_space_pages.page_number == 0 ||
            definition.owned_pages.page_number != definition.free_space_pages.page_number) {
            unsupported_table_write(file,
                                    table.name,
                                    "Overflow row allocation requires table usage maps to share one declaration page");
        }

        auto header_page = file.read_page(row_id.page_number);
        const DataPage header_data_page = validate_data_page(file,
                                                            table.name,
                                                            row_id.page_number,
                                                            table.definition_page_number,
                                                            header_page);
        if (row_id.row_number >= header_data_page.rows.size()) {
            throw_row_error(DatabaseErrorCode::page_out_of_range,
                            file,
                            table.name,
                            row_id,
                            "RowId row is outside its data page");
        }
        const DataPageRow header_row = header_data_page.rows[row_id.row_number];
        if (header_row.bytes.size() < 4) {
            throw_row_error(DatabaseErrorCode::unsupported_operation,
                            file,
                            table.name,
                            row_id,
                            "Row header is too small to store an overflow pointer");
        }

        std::map<std::uint64_t, std::vector<std::uint8_t>> updated_pages;
        updated_pages.emplace(row_id.page_number, std::move(header_page));
        const auto free_page_list = read_usage_map_pages(file, definition.free_space_pages);
        std::optional<std::uint64_t> overflow_page_number;
        for (auto iterator = free_page_list.rbegin(); iterator != free_page_list.rend(); ++iterator) {
            const std::uint64_t candidate_page_number = *iterator;
            if (owned_pages.find(candidate_page_number) == owned_pages.end()) {
                throw_table_error(DatabaseErrorCode::corrupted_file,
                                  file,
                                  table.name,
                                  "Free-space map references a page not owned by the table",
                                  candidate_page_number);
            }

            const auto existing_page = updated_pages.find(candidate_page_number);
            auto candidate_page = existing_page == updated_pages.end()
                                      ? file.read_page(candidate_page_number)
                                      : existing_page->second;
            validate_data_page(file,
                               table.name,
                               candidate_page_number,
                               table.definition_page_number,
                               candidate_page);
            if (!row_fits_on_data_page(file, table.name, candidate_page, encoded_row.bytes.size())) {
                continue;
            }
            if (existing_page == updated_pages.end()) {
                updated_pages.emplace(candidate_page_number, std::move(candidate_page));
            }
            overflow_page_number = candidate_page_number;
            break;
        }

        std::vector<std::uint64_t> appended_data_page_numbers;
        if (!overflow_page_number) {
            const std::uint64_t next_page_number = long_value_pages.next_page_number;
            if (next_page_number >= 0x1000000) {
                unsupported_table_write(file,
                                        table.name,
                                        "Jet4 data page exceeds the three-byte page reference limit");
            }
            overflow_page_number = next_page_number;
            updated_pages.emplace(*overflow_page_number,
                                  new_data_page(file.format().page_size, table.definition_page_number));
            appended_data_page_numbers.push_back(*overflow_page_number);
        }

        auto& overflow_page = updated_pages.at(*overflow_page_number);
        const std::uint16_t overflow_row_number =
            append_row_to_page(overflow_page, encoded_row.bytes, data_page_deleted_row_mask);

        auto& updated_header_page = updated_pages.at(row_id.page_number);
        const std::uint64_t row_location_offset =
            data_page_row_directory_offset + static_cast<std::uint64_t>(row_id.row_number) * 2;
        const std::uint16_t row_location = read_u16(updated_header_page, row_location_offset);
        const std::uint16_t header_offset = static_cast<std::uint16_t>(row_location & data_page_row_offset_mask);
        if (header_offset != header_row.start_offset || header_row.end_offset > updated_header_page.size() ||
            header_row.end_offset - header_offset < 4) {
            throw_row_error(DatabaseErrorCode::corrupted_file,
                            file,
                            table.name,
                            row_id,
                            "Row header bounds cannot hold an overflow pointer");
        }
        if (overflow_row_number > 0xff || *overflow_page_number >= 0x1000000) {
            unsupported_table_write(file, table.name, "Jet4 overflow pointer exceeds its encoded limits");
        }
        updated_header_page[header_offset] = static_cast<std::uint8_t>(overflow_row_number);
        write_u24_le(updated_header_page, header_offset + 1, static_cast<std::uint32_t>(*overflow_page_number));
        std::fill(updated_header_page.begin() + static_cast<std::ptrdiff_t>(header_offset + 4),
                  updated_header_page.begin() + static_cast<std::ptrdiff_t>(header_row.end_offset),
                  0);
        write_u16(updated_header_page,
                  row_location_offset,
                  static_cast<std::uint16_t>(row_location | data_page_overflow_row_mask));

        std::set<std::uint64_t> new_owned_pages(appended_data_page_numbers.begin(),
                                                appended_data_page_numbers.end());
        std::set<std::uint64_t> add_free_pages;
        std::set<std::uint64_t> remove_free_pages;
        const bool more_row_fits = row_fits_on_data_page(file, table.name, overflow_page, encoded_row.bytes.size());
        if (!appended_data_page_numbers.empty() && more_row_fits) {
            add_free_pages.insert(*overflow_page_number);
        } else if (appended_data_page_numbers.empty() && !more_row_fits) {
            remove_free_pages.insert(*overflow_page_number);
        }

        if (!new_owned_pages.empty() || !add_free_pages.empty() || !remove_free_pages.empty()) {
            const auto existing_usage_page = updated_pages.find(definition.owned_pages.page_number);
            if (existing_usage_page == updated_pages.end()) {
                updated_pages.emplace(definition.owned_pages.page_number,
                                      file.read_page(definition.owned_pages.page_number));
            }
            auto& usage_page = updated_pages.at(definition.owned_pages.page_number);
            update_inline_usage_map_row(file,
                                        table.name,
                                        usage_page,
                                        definition.owned_pages,
                                        new_owned_pages,
                                        {},
                                        true);
            update_inline_usage_map_row(file,
                                        table.name,
                                        usage_page,
                                        definition.free_space_pages,
                                        add_free_pages,
                                        remove_free_pages,
                                        false);
        }

        update_long_value_usage_maps(file,
                                     table.name,
                                     definition,
                                     long_value_pages.pages_by_column,
                                     updated_pages);
        std::vector<std::uint64_t> appended_page_numbers;
        for (const auto& page : long_value_pages.pages) {
            appended_page_numbers.push_back(page.first);
        }
        appended_page_numbers.insert(appended_page_numbers.end(),
                                     appended_data_page_numbers.begin(),
                                     appended_data_page_numbers.end());
        if (!appended_page_numbers.empty()) {
            updated_pages[1] = mark_global_pages_allocated(file, appended_page_numbers);
        }

        write_pages_and_refresh(context,
                                file,
                                table.name,
                                row_id.row_number,
                                row_id.page_number,
                                [&](PageFileWriter& writer) {
                                    for (const auto& page : long_value_pages.pages) {
                                        if (writer.append_page(page.second) != page.first) {
                                            throw_table_error(DatabaseErrorCode::io_error,
                                                              file,
                                                              table.name,
                                                              "Long-value page was appended at an unexpected page number",
                                                              page.first);
                                        }
                                    }
                                    for (const std::uint64_t page_number : appended_data_page_numbers) {
                                        if (writer.append_page(updated_pages.at(page_number)) != page_number) {
                                            throw_table_error(DatabaseErrorCode::io_error,
                                                              file,
                                                              table.name,
                                                              "Overflow data page was appended at an unexpected page number",
                                                              page_number);
                                        }
                                    }
                                    for (const auto& entry : updated_pages) {
                                        if (entry.first < file.page_count()) {
                                            writer.write_page(entry.first, entry.second);
                                        }
                                    }
                                });
    } catch (const DatabaseError& error) {
        rethrow_with_table_context(context.file, table.name, error, row_id.row_number);
    }
}

void delete_jet4_row(Jet4WriteContext& context, std::size_t table_index, RowId row_id) {
    if (!context.writable) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Database was opened read-only; reopen it in read-write mode to delete rows",
                            context.file.path());
    }
    std::lock_guard<std::mutex> write_lock(context.write_mutex);
    if (!context.write_state_valid) {
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Database write state is invalid after a previous write failure; reopen and inspect the file",
                            context.file.path());
    }
    if (table_index >= context.schema.tables.size()) {
        throw std::out_of_range("Table schema index is outside the database catalog");
    }
    const auto& table = context.schema.tables[table_index];
    try {
        const PageFile file = PageFile::open(context.file.path());
        if (file.page_count() != context.file.page_count()) {
            throw_table_error(DatabaseErrorCode::io_error,
                              file,
                              table.name,
                              "Database file size changed outside this writable handle");
        }
        const TableDefinition definition = read_current_table_definition(file, table);
        validate_unindexed_local_table(file, table, definition);
        if (row_id.page_number >= file.page_count()) {
            throw_row_error(DatabaseErrorCode::page_out_of_range,
                            file,
                            table.name,
                            row_id,
                            "RowId page is outside the database");
        }
        const auto owned_pages = read_usage_map_pages(file, definition.owned_pages);
        if (std::find(owned_pages.begin(), owned_pages.end(), row_id.page_number) == owned_pages.end()) {
            throw_row_error(DatabaseErrorCode::unsupported_operation,
                            file,
                            table.name,
                            row_id,
                            "RowId page is not owned by this table");
        }

        auto page = file.read_page(row_id.page_number);
        const DataPage data_page = validate_data_page(file,
                                                     table.name,
                                                     row_id.page_number,
                                                     table.definition_page_number,
                                                     page);
        if (row_id.row_number >= data_page.rows.size()) {
            throw_row_error(DatabaseErrorCode::page_out_of_range,
                            file,
                            table.name,
                            row_id,
                            "RowId row is outside its data page");
        }
        if (data_page.rows[row_id.row_number].deleted) {
            return;
        }
        if (definition.row_count == 0) {
            throw_table_error(DatabaseErrorCode::corrupted_file,
                              file,
                              table.name,
                              "Table row count is zero while a live row is present",
                              table.definition_page_number);
        }

        TableRowReader row_reader(file, table.name, definition);
        (void)row_reader.read_at(row_id.page_number, row_id.row_number);

        const std::uint64_t row_location_offset =
            data_page_row_directory_offset + static_cast<std::uint64_t>(row_id.row_number) * 2;
        const std::uint16_t row_location = read_u16(page, row_location_offset);
        write_u16(page,
                  row_location_offset,
                  static_cast<std::uint16_t>(row_location | data_page_deleted_row_mask |
                                             data_page_overflow_row_mask));

        auto updated_definition_page = file.read_page(table.definition_page_number);
        write_u32(updated_definition_page, 16, definition.row_count - 1);
        write_pages_and_refresh(context,
                                file,
                                table.name,
                                row_id.row_number,
                                row_id.page_number,
                                [&](PageFileWriter& writer) {
                                    writer.write_page(row_id.page_number, page);
                                    writer.write_page(table.definition_page_number, updated_definition_page);
                                });
        context.schema.tables[table_index].definition.row_count = definition.row_count - 1;
    } catch (const DatabaseError& error) {
        rethrow_with_table_context(context.file, table.name, error, row_id.row_number);
    }
}

}
