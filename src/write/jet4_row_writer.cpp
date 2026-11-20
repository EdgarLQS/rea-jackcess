#include "jet4_row_writer.hpp"

#include "jet4_fixed_numeric.hpp"
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
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint64_t data_page_row_count_offset = 12;
constexpr std::uint64_t data_page_row_directory_offset = 14;
constexpr std::uint64_t data_page_table_definition_offset = 4;
constexpr std::uint16_t data_page_row_offset_mask = 0x1fff;
constexpr std::uint16_t data_page_deleted_row_mask = 0x8000;
constexpr std::uint16_t data_page_overflow_row_mask = 0x4000;

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
        const auto* numeric_type = jet4_fixed_numeric_type(column.type);
        const auto* variable_type = jet4_variable_type(column.type);
        if ((numeric_type == nullptr) == (variable_type == nullptr)) {
            unsupported_column_write(file, table, column, "Row writing encountered an unsupported column type");
        }
        if (numeric_type != nullptr) {
            if ((column.flags & 0x01) == 0 || column.length != numeric_type->byte_size) {
                unsupported_column_write(file,
                                         table,
                                         column,
                                         "Fixed numeric column has an unsupported Jet4 storage layout");
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

        if ((column.flags & 0x01) != 0 || column.variable_table_index >= variable_columns.size() ||
            variable_columns[column.variable_table_index] != nullptr || column.length == 0 ||
            column.length > variable_type->maximum_byte_size) {
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

std::vector<std::uint8_t> encode_utf8_text(const PageFile& file,
                                           const ReadableTableSchema& table,
                                           const ColumnDefinition& column,
                                           const std::string& value) {
    std::vector<std::uint8_t> encoded;
    encoded.reserve(std::min<std::size_t>(value.size(), column.length));
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
        if (code_unit_count > column.length / 2) {
            invalid_column_value(file, table, column, "TEXT value exceeds the column's 255-character limit");
        }
    }
    return encoded;
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
    for (std::size_t index = 0; index < definition.columns.size(); ++index) {
        auto& column = definition.columns[index];
        const auto& cached_column = cached_definition.columns[index];
        if (column.name != cached_column.name || column.column_number != cached_column.column_number ||
            column.variable_table_index != cached_column.variable_table_index ||
            column.column_id != cached_column.column_id || column.type != cached_column.type ||
            column.precision != cached_column.precision || column.scale != cached_column.scale ||
            column.flags != cached_column.flags || column.extended_flags != cached_column.extended_flags ||
            column.fixed_data_offset != cached_column.fixed_data_offset || column.length != cached_column.length) {
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
    if (!access_version || *access_version != "08.50") {
        unsupported_table_write(file,
                                table.name,
                                "Row modification currently supports Jet4 V2000 databases only");
    }
    if (!definition.indexes.empty() || !definition.physical_indexes.empty()) {
        unsupported_table_write(file,
                                table.name,
                                "Row modification currently does not maintain user indexes");
    }
}

std::vector<std::uint8_t> encode_row(const PageFile& file,
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

        const auto* numeric_type = jet4_fixed_numeric_type(column.type);
        if (numeric_type != nullptr) {
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
                default:
                    break;
            }
            if (!value_encoded) {
                invalid_column_value(file,
                                     table,
                                     column,
                                     "Row value does not match its fixed numeric column type");
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
                auto encoded = encode_utf8_text(file, table, column, *text_value);
                if (encoded.size() > column.length) {
                    invalid_column_value(file, table, column, "TEXT value exceeds the column's byte limit");
                }
                variable_values[column.variable_table_index] = std::move(encoded);
            } else {
                const auto* binary_value = std::get_if<BlobValue>(&value);
                if (binary_value == nullptr) {
                    invalid_column_value(file, table, column, "Row value does not match its BINARY column type");
                }
                if (binary_value->bytes.size() > column.length) {
                    invalid_column_value(file, table, column, "BINARY value exceeds the column's byte limit");
                }
                variable_values[column.variable_table_index] = binary_value->bytes;
            }
        } else {
            invalid_column_value(file, table, column, "Row value uses an unsupported Jet4 column type");
        }
        null_mask[column.column_number / 8] |=
            static_cast<std::uint8_t>(1u << (column.column_number % 8));
    }

    if (!variable_columns.empty()) {
        std::vector<std::uint16_t> variable_offsets(variable_columns.size());
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
    return row;
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
                                const std::vector<std::uint8_t>& row) {
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
    write_u16(page, data_page_row_directory_offset + static_cast<std::uint64_t>(row_count) * 2, start);
    write_u16(page, data_page_row_count_offset, static_cast<std::uint16_t>(row_count + 1));
    const std::uint16_t free_space = read_u16(page, 2);
    write_u16(page, 2, static_cast<std::uint16_t>(free_space - row.size() - 2));
    return row_count;
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
        std::vector<std::vector<std::uint8_t>> encoded_rows;
        encoded_rows.reserve(rows.size());
        for (const auto& row : rows) {
            encoded_rows.push_back(encode_row(file, table, definition, row, fixed_data_size));
        }
        const std::uint64_t maximum_row_size =
            file.format().page_size - data_page_row_directory_offset - 2;
        for (const auto& encoded_row : encoded_rows) {
            if (encoded_row.size() > maximum_row_size) {
                unsupported_table_write(file, table.name, "Encoded row does not fit on a Jet4 data page");
            }
        }
        const auto largest_row = std::max_element(encoded_rows.begin(), encoded_rows.end(), [](const auto& left,
                                                                                              const auto& right) {
            return left.size() < right.size();
        });
        const std::size_t largest_row_size = largest_row->size();

        const auto owned_page_list = read_usage_map_pages(file, definition.owned_pages);
        const auto free_page_list = read_usage_map_pages(file, definition.free_space_pages);
        const std::set<std::uint64_t> owned_pages(owned_page_list.begin(), owned_page_list.end());
        std::set<std::uint64_t> free_pages(free_page_list.begin(), free_page_list.end());
        std::set<std::uint64_t> removed_free_pages;
        std::map<std::uint64_t, std::vector<std::uint8_t>> data_pages;
        std::vector<std::uint64_t> appended_page_numbers;
        std::uint64_t next_page_number = file.page_count();
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
                !row_fits_on_data_page(file, table.name, data_pages.at(*active_page_number), row.size())) {
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
                active_page_number = page_number;
            }

            auto& page = data_pages.at(*active_page_number);
            const std::uint16_t row_number = append_row_to_page(page, row);
            row_ids.push_back({*active_page_number, row_number});
            if (row_fits_on_data_page(file, table.name, page, row.size())) {
                free_pages.insert(*active_page_number);
                removed_free_pages.erase(*active_page_number);
            } else {
                free_pages.erase(*active_page_number);
                removed_free_pages.insert(*active_page_number);
            }
        }

        std::vector<std::uint64_t> newly_free_pages;
        for (const std::uint64_t page_number : appended_page_numbers) {
            if (free_pages.find(page_number) != free_pages.end()) {
                newly_free_pages.push_back(page_number);
            }
        }
        std::set<std::uint64_t> new_owned_pages(appended_page_numbers.begin(), appended_page_numbers.end());
        std::set<std::uint64_t> add_free_pages(newly_free_pages.begin(), newly_free_pages.end());

        auto updated_usage_page = file.read_page(definition.owned_pages.page_number);
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
                                    writer.write_page(definition.owned_pages.page_number, updated_usage_page);
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
        const std::vector<std::uint8_t> encoded_row = encode_row(file,
                                                                table,
                                                                definition,
                                                                values,
                                                                fixed_data_size);

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
        const auto& row = data_page.rows[row_id.row_number];
        if (row.deleted || row.overflow) {
            throw_row_error(DatabaseErrorCode::unsupported_operation,
                            file,
                            table.name,
                            row_id,
                            "RowId does not refer to a live, non-overflow row");
        }
        if (row.bytes.size() != encoded_row.size()) {
            throw_row_error(DatabaseErrorCode::unsupported_operation,
                            file,
                            table.name,
                            row_id,
                            "In-place row update requires an unchanged encoded row size");
        }
        TableRowReader row_reader(file, table.name, definition);
        (void)row_reader.read_at(row_id.page_number, row_id.row_number);
        std::copy(encoded_row.begin(),
                  encoded_row.end(),
                  page.begin() + static_cast<std::ptrdiff_t>(row.start_offset));

        write_pages_and_refresh(context,
                                file,
                                table.name,
                                row_id.row_number,
                                row_id.page_number,
                                [&](PageFileWriter& writer) {
                                    writer.write_page(row_id.page_number, page);
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
