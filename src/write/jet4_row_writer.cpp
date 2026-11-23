#include "jet4_row_writer.hpp"

#include "jet4_fixed_type.hpp"
#include "jet4_global_usage_map.hpp"
#include "jet4_index_entry.hpp"
#include "jet4_relationship_creator.hpp"
#include "jet4_variable_type.hpp"

#include "../util/little_endian.hpp"
#include "../catalog/property_map.hpp"
#include "../catalog/system_catalog.hpp"
#include "../catalog/table_definition.hpp"
#include "../catalog/row_reader.hpp"
#include "../catalog/usage_map.hpp"
#include "../index/index_cursor.hpp"
#include "../page/data_page.hpp"
#include "../page/page_types.hpp"
#include "../page/page_writer.hpp"
#include "../value/jet4_value.hpp"
#include "../value/text_codec.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
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
#include <type_traits>
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
    PageWriteUndo undo(file.path(), file.page_count(), file.format().page_size);
    try {
        {
            auto writer = PageFileWriter::open(file.path(), context.undo ? context.undo : &undo);
            if (writer.page_count() != file.page_count()) {
                throw_table_error(DatabaseErrorCode::io_error,
                                  file,
                                  table_name,
                                  "Database page count changed before write pages were written",
                                  page_number);
            }
            std::forward<WriteOperation>(write_operation)(writer);
            writer.flush();
        }
        context.file = PageFile::open(file.path());
    } catch (...) {
        try {
            if (!context.undo) {
                undo.rollback();
            }
        } catch (...) {
            context.write_state_valid = false;
            throw;
        }
        try {
            throw;
        } catch (const DatabaseError& error) {
            rethrow_with_table_context(file, table_name, error, row_number);
        }
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
                                  const TableDefinition& definition,
                                  bool preserve_raw = false) {
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
        if (preserve_raw) {
            if ((column.flags & 1) != 0) {
                data_size = std::max(data_size, static_cast<std::uint64_t>(column.fixed_data_offset) + column.length);
            } else {
                if (column.variable_table_index >= variable_columns.size() ||
                    variable_columns[column.variable_table_index] != nullptr) {
                    unsupported_column_write(file, table, column, "Invalid catalog variable-column layout");
                }
                variable_columns[column.variable_table_index] = &column;
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
    try {
        return jet4_money_scaled_value(text);
    } catch (const std::invalid_argument& error) {
        invalid_column_value(file, table, column, error.what());
    }
}

std::array<std::uint8_t, 16> encode_guid(const PageFile& file,
                                        const ReadableTableSchema& table,
                                        const ColumnDefinition& column,
                                        std::string_view text) {
    try {
        return jet4_guid_bytes(text);
    } catch (const std::invalid_argument& error) {
        invalid_column_value(file, table, column, error.what());
    }
}

// Converts the ISO_LOCAL_DATE_TIME text the reader produces into the Access date serial Jet4
// stores, reporting malformed or out-of-range text against the column.
double encode_date_serial(const PageFile& file,
                          const ReadableTableSchema& table,
                          const ColumnDefinition& column,
                          std::string_view text) {
    std::string error;
    const auto serial = jet4_date_serial(text, &error);
    if (!serial) {
        invalid_column_value(file, table, column, error.c_str());
    }
    return *serial;
}

std::vector<std::uint8_t> encode_utf8_text(const PageFile& file,
                                           const ReadableTableSchema& table,
                                           const ColumnDefinition& column,
                                           const std::string& value,
                                           std::uint64_t maximum_code_units,
                                           const char* size_error) {
    std::u16string units;
    try {
        units = utf8_to_utf16(value, maximum_code_units, size_error);
    } catch (const std::invalid_argument& error) {
        invalid_column_value(file, table, column, error.what());
    }
    std::vector<std::uint8_t> encoded;
    encoded.reserve(units.size() * 2);
    for (const auto unit : units) {
        encoded.push_back(static_cast<std::uint8_t>(unit));
        encoded.push_back(static_cast<std::uint8_t>(unit >> 8));
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
        column.default_expression = cached_column.default_expression;
        column.validation_rule = cached_column.validation_rule;
        column.validation_text = cached_column.validation_text;
    }
    definition.table_validation_rule = cached_definition.table_validation_rule;
    definition.table_validation_text = cached_definition.table_validation_text;
    return definition;
}

void validate_local_table(const PageFile& file,
                          const ReadableTableSchema& table,
                          const TableDefinition& definition,
                          bool allow_catalog_table = false) {
    if (file.format().version != EngineVersion::jet4 || file.format().page_size != 4096) {
        unsupported_table_write(file, table.name, "Row modification requires a Jet4 4 KB page database");
    }
    if (table.is_linked || (!table.is_user_table && !allow_catalog_table)) {
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
    // V2000 templates carry AccessVersion 08.50 and V2003 templates 09.50. Files written by
    // third-party Jet4 drivers (ArcGIS, for example) may carry no AccessVersion property at all;
    // they still use the Jet4 layout already validated above, so only a known-but-foreign marker
    // is rejected.
    if (access_version && *access_version != "08.50" && *access_version != "09.50") {
        unsupported_table_write(file,
                                table.name,
                                "Row modification currently supports Jet4 V2000/V2003 markers and Jet4 files without an AccessVersion property");
    }
    for (const auto& physical : definition.physical_indexes) {
        if (physical.columns.empty() || physical.root_page_number == 0) {
            unsupported_table_write(file, table.name, "Index has no columns or root page");
        }
        for (const auto& indexed_column : physical.columns) {
            if (indexed_column.column_number >= definition.columns.size()) {
                throw_table_error(DatabaseErrorCode::corrupted_file, file, table.name,
                                  "Index references a column outside the table definition");
            }
            if (!jet4_index_key_supported(definition.columns[indexed_column.column_number])) {
                unsupported_table_write(file, table.name,
                                        "Row modification cannot encode this index column type or text sort order");
            }
        }
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
                      std::uint64_t fixed_data_size,
                      const EncodedTableRow* original = nullptr,
                      const std::vector<std::uint8_t>* property_bytes = nullptr,
                      bool catalog_object = false) {
    if (!original && values.size() != definition.columns.size()) {
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
        if (original) {
            const auto& encoded = original->columns.at(index);
            bool present = encoded.bytes.has_value();
            if ((column.flags & 1) == 0) {
                variable_columns[column.variable_table_index] = &column;
                if (column.name == "LvProp" && column.type == 0x0b) {
                    present = !property_bytes->empty();
                    if (present) {
                        std::vector<std::uint8_t> long_value(12, 0);
                        constexpr std::size_t max_inline_long_value_size = 64;
                        if (property_bytes->size() <= max_inline_long_value_size) {
                            write_u32(long_value, 0, long_value_inline_flag |
                                                    static_cast<std::uint32_t>(property_bytes->size()));
                            long_value.insert(long_value.end(), property_bytes->begin(), property_bytes->end());
                        } else {
                            long_value_payloads[column.variable_table_index] = *property_bytes;
                        }
                        variable_values[column.variable_table_index] = std::move(long_value);
                    }
                } else {
                    variable_values[column.variable_table_index] = encoded.bytes;
                }
            } else if (jet4_is_boolean_type(column.type)) {
                present = encoded.boolean_value;
            } else if (present) {
                if (encoded.bytes->size() != column.length) {
                    invalid_column_value(file, table, column, "Invalid catalog fixed-column length");
                }
                std::copy(encoded.bytes->begin(), encoded.bytes->end(),
                          row.begin() + 2 + column.fixed_data_offset);
            }
            if (present) {
                null_mask[column.column_number / 8] |= static_cast<std::uint8_t>(1u << (column.column_number % 8));
            }
            continue;
        }
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

        if (catalog_object && column.type == 0x08 && (column.flags & 1) != 0 && column.length == 8) {
            const auto* serial = std::get_if<double>(&value);
            if (!serial) {
                invalid_column_value(file, table, column, "Catalog date requires an Access date serial");
            }
            std::uint64_t bits;
            std::memcpy(&bits, serial, sizeof(bits));
            write_u64(row, 2 + column.fixed_data_offset, bits);
            null_mask[column.column_number / 8] |= static_cast<std::uint8_t>(1u << (column.column_number % 8));
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
                case jet4_date_type_code:
                    if (const auto* date_value = std::get_if<DateTimeValue>(&value)) {
                        const double serial = encode_date_serial(file, table, column, date_value->iso8601);
                        std::uint64_t bits = 0;
                        std::memcpy(&bits, &serial, sizeof(bits));
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
    // Pages allocated by this operation; the rest were reused from the free-space map.
    std::set<std::uint64_t> appended_pages;
    std::map<std::uint16_t, std::set<std::uint64_t>> pages_by_column;
    // Pages that still belong to the column but no longer have room for another value.
    std::map<std::uint16_t, std::set<std::uint64_t>> full_pages_by_column;
    std::uint64_t next_page_number;
};

// Jackcess reuses the highest owned long-value page that is also listed in the
// column's free-space map (see TableImpl.findFreeRowSpace).
std::vector<std::uint64_t> reusable_long_value_pages(const PageFile& file,
                                                     const ColumnDefinition& column) {
    if (!column.long_value_owned_pages || !column.long_value_free_space_pages) {
        return {};
    }
    const auto owned = read_usage_map_pages(file, *column.long_value_owned_pages);
    const auto free = read_usage_map_pages(file, *column.long_value_free_space_pages);
    std::vector<std::uint64_t> candidates;
    std::set_intersection(owned.begin(),
                          owned.end(),
                          free.begin(),
                          free.end(),
                          std::back_inserter(candidates));
    std::reverse(candidates.begin(), candidates.end());
    return candidates;
}

void stage_long_value_pages(const PageFile& file,
                            const std::string& table_name,
                            const TableDefinition& definition,
                            EncodedRow& row,
                            LongValuePageSet& result) {
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
        const bool chained = page_count > 1;
        const std::uint32_t type_flag = chained ? 0 : long_value_other_page_flag;
        const auto write_definition = [&](std::uint64_t page_number, std::uint8_t row_number) {
            if (pending.definition_offset > row.bytes.size() ||
                12 > row.bytes.size() - pending.definition_offset) {
                throw_table_error(DatabaseErrorCode::corrupted_file,
                                  file,
                                  table_name,
                                  "Long-value definition extends beyond its encoded row");
            }
            write_u32(row.bytes,
                      pending.definition_offset,
                      static_cast<std::uint32_t>(pending.bytes.size()) | type_flag);
            row.bytes[pending.definition_offset + 4] = row_number;
            write_u24_le(row.bytes,
                         pending.definition_offset + 5,
                         static_cast<std::uint32_t>(page_number));
            write_u32(row.bytes, pending.definition_offset + 8, 0);
        };

        bool reused_page = false;
        if (!chained) {
            const auto column = std::find_if(definition.columns.begin(),
                                             definition.columns.end(),
                                             [&](const auto& candidate) {
                                                 return candidate.column_number == pending.column_number;
                                             });
            if (column == definition.columns.end() || !jet4_is_long_value_type(column->type)) {
                throw_table_error(DatabaseErrorCode::corrupted_file,
                                  file,
                                  table_name,
                                  "Long-value payload references a non-long-value column");
            }
            for (const std::uint64_t page_number : reusable_long_value_pages(file, *column)) {
                const auto existing = result.pages.find(page_number);
                const std::vector<std::uint8_t> page_bytes =
                    existing != result.pages.end() ? existing->second : file.read_page(page_number);
                if (page_bytes.empty() || page_bytes[0] != static_cast<std::uint8_t>(PageType::data) ||
                    !row_fits_on_data_page(file, table_name, page_bytes, pending.bytes.size())) {
                    continue;
                }
                auto& page = existing != result.pages.end()
                                 ? existing->second
                                 : result.pages.emplace(page_number, std::move(page_bytes)).first->second;
                const std::uint16_t row_number = append_row_to_page(page, pending.bytes);
                result.pages_by_column[pending.column_number].insert(page_number);
                if (!row_fits_on_data_page(file, table_name, page, 1)) {
                    result.full_pages_by_column[pending.column_number].insert(page_number);
                }
                write_definition(page_number, static_cast<std::uint8_t>(row_number));
                reused_page = true;
                break;
            }
        }
        if (reused_page) {
            continue;
        }

        if (result.next_page_number >= 0x1000000 ||
            page_count > 0x1000000 - result.next_page_number) {
            unsupported_table_write(file, table_name, "Jet4 long-value page exceeds the three-byte page reference limit");
        }
        const std::uint64_t first_page = result.next_page_number;
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
            result.appended_pages.insert(page_number);
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
        write_definition(first_page, 0);
        result.next_page_number += page_count;
    }
}

LongValuePageSet allocate_long_value_pages(const PageFile& file,
                                           const std::string& table_name,
                                           const TableDefinition& definition,
                                           EncodedRow& row,
                                           std::uint64_t first_page_number) {
    LongValuePageSet result{{}, {}, {}, {}, first_page_number};
    stage_long_value_pages(file, table_name, definition, row, result);
    return result;
}

LongValuePageSet allocate_long_value_pages(const PageFile& file,
                                           const std::string& table_name,
                                           const TableDefinition& definition,
                                           std::vector<EncodedRow>& rows,
                                           std::uint64_t first_page_number) {
    LongValuePageSet result{{}, {}, {}, {}, first_page_number};
    for (auto& row : rows) {
        stage_long_value_pages(file, table_name, definition, row, result);
    }
    return result;
}

struct UsageBitmapAllocation {
    std::uint64_t first_page;
    std::vector<std::uint64_t> pages;
};

void update_usage_map_row(const PageFile& file,
                          const std::string& table_name,
                          std::map<std::uint64_t, std::vector<std::uint8_t>>& staged_pages,
                          const UsageMapReference& reference,
                          const std::set<std::uint64_t>& add_pages,
                          const std::set<std::uint64_t>& remove_pages,
                          bool convert_empty_reference_map,
                          UsageBitmapAllocation* allocation);

[[noreturn]] void throw_long_value_reference_error(const PageFile& file,
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

DataPage read_long_value_reference_page(const PageFile& file,
                                        const std::string& table_name,
                                        const EncodedTableRow& row,
                                        const ColumnDefinition& column,
                                        std::uint64_t page_number,
                                        const std::map<std::uint64_t, std::vector<std::uint8_t>>* staged_pages) {
    try {
        if (staged_pages != nullptr) {
            const auto staged = staged_pages->find(page_number);
            if (staged != staged_pages->end()) {
                return parse_data_page(file, page_number, staged->second);
            }
        }
        return read_data_page(file, page_number);
    } catch (const DatabaseError& error) {
        auto context = error.context();
        context.table_name = table_name;
        context.row_number = row.row_number;
        context.column_number = column.column_number;
        context.column_name = column.name;
        throw DatabaseError(error.code(), error.what(), file.path(), error.page_number(), std::move(context));
    }
}

std::set<std::uint64_t> long_value_pages_referenced_by_row(const PageFile& file,
                                                           const std::string& table_name,
                                                           const EncodedTableRow& row,
                                                           const ColumnDefinition& column,
                                                           const std::map<std::uint64_t, std::vector<std::uint8_t>>*
                                                               staged_pages = nullptr) {
    if (column.column_number >= row.columns.size() || !row.columns[column.column_number].bytes) {
        return {};
    }
    const auto& definition = *row.columns[column.column_number].bytes;
    if (definition.empty()) {
        return {};
    }
    if (definition.size() < 4) {
        throw_long_value_reference_error(file, table_name, row, column,
                                         "Long-value definition is shorter than its type header");
    }

    const std::uint32_t length_with_flags = read_u32(definition, 0);
    const std::uint32_t value_type = length_with_flags & 0xc0000000u;
    std::uint64_t remaining = length_with_flags & long_value_length_mask;
    if (value_type == long_value_inline_flag) {
        if (definition.size() < 12 || remaining > definition.size() - 12) {
            throw_long_value_reference_error(file, table_name, row, column,
                                             "Inline long-value data is truncated");
        }
        return {};
    }
    if (definition.size() != 12) {
        throw_long_value_reference_error(file, table_name, row, column,
                                         "External long-value definition has an invalid length");
    }

    std::uint16_t row_number = definition[4];
    std::uint64_t page_number = read_u24_le(definition, 5);
    const auto is_available_page = [&](std::uint64_t candidate) {
        return candidate != 0 &&
               (candidate < file.page_count() ||
                (staged_pages != nullptr && staged_pages->count(candidate) != 0));
    };
    if (!is_available_page(page_number)) {
        throw_long_value_reference_error(file, table_name, row, column,
                                         "Long-value pointer references an invalid page", page_number);
    }

    std::set<std::uint64_t> referenced_pages;
    if (value_type == long_value_other_page_flag) {
        const auto page = read_long_value_reference_page(file,
                                                         table_name,
                                                         row,
                                                         column,
                                                         page_number,
                                                         staged_pages);
        if (row_number >= page.rows.size() || page.rows[row_number].deleted) {
            throw_long_value_reference_error(file, table_name, row, column,
                                             "Long-value pointer references an invalid row", page_number);
        }
        if (page.rows[row_number].bytes.size() < remaining) {
            throw_long_value_reference_error(file, table_name, row, column,
                                             "External long-value data is truncated", page_number);
        }
        referenced_pages.insert(page_number);
        return referenced_pages;
    }
    if (value_type != 0) {
        throw_long_value_reference_error(file, table_name, row, column,
                                         "Long-value type flags are unsupported");
    }

    std::set<std::pair<std::uint64_t, std::uint16_t>> visited;
    while (remaining > 0) {
        if (!is_available_page(page_number)) {
            throw_long_value_reference_error(file, table_name, row, column,
                                             "Long-value continuation references an invalid page", page_number);
        }
        if (!visited.emplace(page_number, row_number).second) {
            throw_long_value_reference_error(file, table_name, row, column,
                                             "Long-value continuation contains a cycle", page_number);
        }
        const auto page = read_long_value_reference_page(file,
                                                         table_name,
                                                         row,
                                                         column,
                                                         page_number,
                                                         staged_pages);
        if (row_number >= page.rows.size() || page.rows[row_number].deleted) {
            throw_long_value_reference_error(file, table_name, row, column,
                                             "Long-value continuation references an invalid row", page_number);
        }
        const auto& chunk = page.rows[row_number].bytes;
        if (chunk.size() < long_value_page_pointer_size) {
            throw_long_value_reference_error(file, table_name, row, column,
                                             "Long-value continuation pointer is truncated", page_number);
        }
        const std::size_t chunk_size = std::min<std::uint64_t>(remaining,
                                                               chunk.size() - long_value_page_pointer_size);
        if (chunk_size == 0) {
            throw_long_value_reference_error(file, table_name, row, column,
                                             "Long-value continuation contains no data", page_number);
        }
        referenced_pages.insert(page_number);
        remaining -= chunk_size;
        if (remaining == 0) {
            break;
        }
        row_number = chunk[0];
        page_number = read_u24_le(chunk, 1);
        if (page_number == 0) {
            throw_long_value_reference_error(file, table_name, row, column,
                                             "Long-value continuation ended before its declared length");
        }
    }
    return referenced_pages;
}

void release_unreferenced_long_value_pages(
    const PageFile& file,
    const std::string& table_name,
    const TableDefinition& definition,
    RowId replaced_row_id,
    const EncodedTableRow& replaced_row,
    const std::vector<std::uint8_t>* replacement_row_bytes,
    const std::map<std::uint64_t, std::vector<std::uint8_t>>& staged_long_value_pages,
    std::map<std::uint64_t, std::vector<std::uint8_t>>& updated_pages) {
    std::map<std::uint16_t, std::set<std::uint64_t>> old_pages_by_column;
    std::set<std::uint64_t> referenced_pages;
    for (const auto& column : definition.columns) {
        if (!jet4_is_long_value_type(column.type) || !column.long_value_owned_pages ||
            !column.long_value_free_space_pages) {
            continue;
        }
        auto pages = long_value_pages_referenced_by_row(file, table_name, replaced_row, column);
        if (!pages.empty()) {
            old_pages_by_column.emplace(column.column_number, std::move(pages));
        }
    }
    if (old_pages_by_column.empty()) {
        return;
    }

    if (replacement_row_bytes != nullptr) {
        const auto replacement = decode_row_layout(file,
                                                   table_name,
                                                   replaced_row_id.page_number,
                                                   replaced_row_id.row_number,
                                                   *replacement_row_bytes,
                                                   definition);
        for (const auto& column : definition.columns) {
            if (!jet4_is_long_value_type(column.type)) {
                continue;
            }
            const auto pages = long_value_pages_referenced_by_row(file,
                                                                  table_name,
                                                                  replacement,
                                                                  column,
                                                                  &staged_long_value_pages);
            referenced_pages.insert(pages.begin(), pages.end());
        }
    }

    // A long-value page can hold rows referenced by different table rows, so release it only
    // after checking the remaining live descriptors and the replacement row's allocations.
    TableRowReader reader(file, table_name, definition);
    while (auto row = reader.next()) {
        if (row->page_number == replaced_row_id.page_number && row->row_number == replaced_row_id.row_number) {
            continue;
        }
        for (const auto& column : definition.columns) {
            if (!jet4_is_long_value_type(column.type)) {
                continue;
            }
            const auto pages = long_value_pages_referenced_by_row(file,
                                                                  table_name,
                                                                  *row,
                                                                  column,
                                                                  &staged_long_value_pages);
            referenced_pages.insert(pages.begin(), pages.end());
        }
    }

    std::set<std::uint64_t> released_pages;
    for (const auto& candidate : old_pages_by_column) {
        const auto column = std::find_if(definition.columns.begin(), definition.columns.end(), [&](const auto& item) {
            return item.column_number == candidate.first;
        });
        if (column == definition.columns.end()) {
            throw_table_error(DatabaseErrorCode::corrupted_file,
                              file,
                              table_name,
                              "Long-value usage map references an unknown column");
        }
        std::set<std::uint64_t> unreferenced;
        std::set_difference(candidate.second.begin(), candidate.second.end(),
                            referenced_pages.begin(), referenced_pages.end(),
                            std::inserter(unreferenced, unreferenced.end()));
        if (unreferenced.empty()) {
            continue;
        }

        const auto owned_pages = read_usage_map_pages(file, *column->long_value_owned_pages);
        if (std::any_of(unreferenced.begin(), unreferenced.end(), [&](std::uint64_t page) {
                return !std::binary_search(owned_pages.begin(), owned_pages.end(), page);
            })) {
            throw_table_error(DatabaseErrorCode::corrupted_file,
                              file,
                              table_name,
                              "Long-value reference is missing from its column usage map");
        }
        update_usage_map_row(file, table_name, updated_pages, *column->long_value_owned_pages,
                             {}, unreferenced, false, nullptr);
        update_usage_map_row(file, table_name, updated_pages, *column->long_value_free_space_pages,
                             {}, unreferenced, false, nullptr);
        released_pages.insert(unreferenced.begin(), unreferenced.end());
    }

    if (!released_pages.empty()) {
        mark_global_pages_free(file, updated_pages,
                               std::vector<std::uint64_t>(released_pages.begin(), released_pages.end()));
    }
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

std::vector<std::uint8_t>& mutable_page(std::map<std::uint64_t, std::vector<std::uint8_t>>& pages,
                                        const PageFile& file,
                                        std::uint64_t page_number) {
    const auto existing = pages.find(page_number);
    if (existing != pages.end()) {
        return existing->second;
    }
    return pages.emplace(page_number, file.read_page(page_number)).first->second;
}

void update_usage_map_row(const PageFile& file,
                          const std::string& table_name,
                          std::map<std::uint64_t, std::vector<std::uint8_t>>& staged_pages,
                          const UsageMapReference& reference,
                          const std::set<std::uint64_t>& add_pages,
                          const std::set<std::uint64_t>& remove_pages,
                          bool convert_empty_reference_map,
                          UsageBitmapAllocation* allocation = nullptr) {
    if (add_pages.empty() && remove_pages.empty()) {
        return;
    }
    auto& usage_page = mutable_page(staged_pages, file, reference.page_number);
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
    constexpr std::size_t bitmap_offset = 4;
    const auto pages_per_bitmap = (file.format().page_size - bitmap_offset) * 8;
    const auto slots = (bytes.size() - 1) / 4;
    const auto change_reference_bit = [&](std::uint64_t page_number, bool add) {
        const auto slot = page_number / pages_per_bitmap;
        if (slot >= slots) {
            if (add) {
                unsupported_table_write(file, table_name, "Usage map has no reference slot for this page");
            }
            return;
        }
        const auto pointer = 1 + slot * 4;
        auto bitmap_page = read_u32(bytes, pointer);
        if (bitmap_page == 0) {
            if (!add) {
                return;
            }
            if (allocation == nullptr ||
                allocation->first_page + allocation->pages.size() >= 0x1000000) {
                unsupported_table_write(file, table_name, "Cannot allocate a usage map bitmap page");
            }
            bitmap_page = static_cast<std::uint32_t>(allocation->first_page + allocation->pages.size());
            auto& bitmap = staged_pages.emplace(bitmap_page,
                                                std::vector<std::uint8_t>(file.format().page_size, 0))
                               .first->second;
            bitmap[0] = static_cast<std::uint8_t>(PageType::usage_map);
            bitmap[1] = 1;
            allocation->pages.push_back(bitmap_page);
            write_u32(bytes, pointer, bitmap_page);
        }
        if (bitmap_page >= file.page_count() &&
            (allocation == nullptr ||
             std::find(allocation->pages.begin(), allocation->pages.end(), bitmap_page) == allocation->pages.end())) {
            throw_table_error(DatabaseErrorCode::corrupted_file, file, table_name,
                              "Usage map references a bitmap outside the database", bitmap_page);
        }
        auto& bitmap = mutable_page(staged_pages, file, bitmap_page);
        if (bitmap[0] != static_cast<std::uint8_t>(PageType::usage_map)) {
            throw_table_error(DatabaseErrorCode::corrupted_file, file, table_name,
                              "Usage map bitmap has an invalid page type", bitmap_page);
        }
        const auto bit = page_number % pages_per_bitmap;
        auto& byte = bitmap[bitmap_offset + bit / 8];
        const auto mask = static_cast<std::uint8_t>(1u << (bit % 8));
        byte = static_cast<std::uint8_t>(add ? byte | mask : byte & ~mask);
    };
    const auto write_declaration = [&] {
        std::copy(bytes.begin(), bytes.end(), usage_page.begin() + declaration.start_offset);
    };
    if (bytes[0] == 1) {
        const auto has_bitmap_page = std::any_of(
            bytes.begin() + 1, bytes.end(), [](std::uint8_t value) { return value != 0; });
        if (has_bitmap_page || !convert_empty_reference_map) {
            for (const auto page : remove_pages) {
                change_reference_bit(page, false);
            }
            for (const auto page : add_pages) {
                change_reference_bit(page, true);
            }
            write_declaration();
            return;
        }
        std::fill(bytes.begin(), bytes.end(), 0);
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
            std::fill(bytes.begin(), bytes.end(), 0);
            bytes[0] = 1;
            for (const auto number : pages) {
                change_reference_bit(number, true);
            }
            write_declaration();
            return;
        }
        first_page = new_first_page;
    }
    write_u32(bytes, 1, static_cast<std::uint32_t>(first_page));
    std::fill(bytes.begin() + 5, bytes.end(), 0);
    for (const std::uint64_t page_number : pages) {
        const std::uint64_t bit_index = page_number - first_page;
        bytes[5 + bit_index / 8] |= static_cast<std::uint8_t>(1u << (bit_index % 8));
    }
    write_declaration();
}


struct Jet4IndexPageUpdates {
    std::map<std::uint64_t, std::vector<std::uint8_t>> pages;
    std::map<std::size_t, std::int64_t> count_deltas;
};

constexpr std::uint8_t index_unique_flag = 0x01;
constexpr std::uint8_t index_ignore_nulls_flag = 0x02;
constexpr std::uint8_t index_required_flag = 0x08;
constexpr std::uint8_t primary_index_type = 1;

bool is_primary_index(const TableDefinition& definition, std::size_t physical_index) {
    return std::any_of(definition.indexes.begin(), definition.indexes.end(), [&](const auto& index) {
        return index.index_data_number == physical_index && index.type == primary_index_type;
    });
}

struct EncodedIndexKey {
    std::vector<std::uint8_t> bytes;
    std::size_t null_count = 0;
};

template <typename ColumnValue>
EncodedIndexKey encode_row_index_key(const PageFile& file, const ReadableTableSchema& table,
                                    const TableDefinition& definition, const PhysicalIndexDefinition& index,
                                    const std::vector<ColumnValue>& values,
                                    const EncodedTableRow* source_row = nullptr) {
    EncodedIndexKey key;
    for (const auto& indexed : index.columns) {
        const auto number = indexed.column_number;
        if (number >= values.size()) {
            throw_table_error(DatabaseErrorCode::corrupted_file, file, table.name,
                              "Indexed column is outside the encoded row", table.definition_page_number);
        }
        try {
            const auto& column = definition.columns.at(number);
            std::vector<std::uint8_t> part;
            if constexpr (std::is_same_v<ColumnValue, EncodedColumnValue>) {
                if (column.type == 0x0c) {
                    if (source_row == nullptr) {
                        unsupported_table_write(file, table.name,
                                                "MEMO index maintenance requires its source row");
                    }
                    part = encode_jet4_index_key(column,
                                                 decode_jet4_value(file, table.name, *source_row, column),
                                                 !indexed.ascending);
                } else {
                    part = encode_jet4_index_key(column, values[number], !indexed.ascending);
                }
            } else {
                part = encode_jet4_index_key(column, values[number], !indexed.ascending);
            }
            if (part.size() == 1 && (part.front() == 0x00 || part.front() == 0xff)) {
                ++key.null_count;
            }
            key.bytes.insert(key.bytes.end(), part.begin(), part.end());
        } catch (const std::invalid_argument& error) {
            unsupported_table_write(file, table.name, error.what());
        }
    }
    key.bytes = truncate_jet4_index_key(std::move(key.bytes));
    return key;
}

bool ignores_index_key(const PhysicalIndexDefinition& index, const EncodedIndexKey& key) {
    return (index.flags & index_ignore_nulls_flag) != 0 && key.null_count == index.columns.size();
}

bool includes_new_index_key(const PageFile& file, const std::string& table_name,
                           const PhysicalIndexDefinition& index, bool primary,
                           const EncodedIndexKey& key) {
    if (ignores_index_key(index, key)) {
        return false;
    }
    if ((primary || (index.flags & index_required_flag) != 0) && key.null_count != 0) {
        unsupported_table_write(file, table_name, "NULL violates a required or primary index");
    }
    return true;
}

// Foreign key descriptors come from the file, so both the index data number and the
// referenced column numbers need a range check before they index a catalog vector.
const PhysicalIndexDefinition& foreign_physical_index(const PageFile& file,
                                                      const std::string& table_name,
                                                      const TableDefinition& definition,
                                                      std::uint32_t index_data_number) {
    if (index_data_number >= definition.physical_indexes.size()) {
        throw_table_error(DatabaseErrorCode::corrupted_file, file, table_name,
                          "Foreign key references an index data number outside the index block");
    }
    return definition.physical_indexes[index_data_number];
}

const ColumnDefinition& foreign_index_column(const PageFile& file, const std::string& table_name,
                                             const TableDefinition& definition,
                                             std::uint16_t column_number) {
    if (column_number >= definition.columns.size()) {
        throw_table_error(DatabaseErrorCode::corrupted_file, file, table_name,
                          "Foreign key index references a column outside the table definition");
    }
    return definition.columns[column_number];
}

// Old row values of the row a write is about, kept so the parent side of a relationship can
// still locate dependent rows after the row itself has been rewritten.
struct CascadeSeed {
    bool captured = false;
    std::vector<Value> values;

    void capture(std::vector<Value> decoded) {
        values = std::move(decoded);
        captured = true;
    }
};

std::vector<Value> decode_row_values(const PageFile& file, const ReadableTableSchema& table,
                                     const TableDefinition& definition, const EncodedTableRow& row) {
    std::vector<Value> values;
    values.reserve(definition.columns.size());
    for (const auto& column : definition.columns) {
        values.push_back(decode_jet4_value(file, table.name, row, column));
    }
    return values;
}

void validate_foreign_keys(const PageFile& file, const TableSchemaCatalog& schema,
                           const ReadableTableSchema& table, const TableDefinition& definition,
                           const EncodedTableRow* old_row, const std::vector<Value>* new_values) {
    constexpr std::uint8_t foreign_index_type = 2;
    constexpr std::uint8_t foreign_parent_type = 1;
    constexpr std::uint8_t foreign_child_type = 2;
    for (const auto& link : definition.indexes) {
        if (link.type != foreign_index_type || link.referenced_index_number < 0) {
            continue;
        }
        const bool parent = link.referenced_index_type == foreign_parent_type;
        if (!parent && link.referenced_index_type != foreign_child_type) {
            throw_table_error(DatabaseErrorCode::corrupted_file, file, table.name,
                              "Foreign key has an invalid endpoint type");
        }
        if ((parent && old_row == nullptr) || (!parent && new_values == nullptr)) {
            continue;
        }
        const auto target = std::find_if(schema.tables.begin(), schema.tables.end(), [&](const auto& other) {
            return other.definition_page_number == link.referenced_table_page_number && !other.is_linked;
        });
        if (target == schema.tables.end()) {
            unsupported_table_write(file, table.name, "Foreign key references an unavailable local table");
        }
        const auto& target_definition = target->definition;
        const auto reciprocal = std::find_if(target_definition.indexes.begin(), target_definition.indexes.end(),
                                            [&](const auto& other) {
            return other.index_number == static_cast<std::uint32_t>(link.referenced_index_number);
        });
        if (reciprocal == target_definition.indexes.end() ||
            reciprocal->type != foreign_index_type ||
            reciprocal->referenced_index_type != (parent ? foreign_child_type : foreign_parent_type) ||
            reciprocal->referenced_table_page_number != table.definition_page_number ||
            reciprocal->referenced_index_number != static_cast<std::int32_t>(link.index_number)) {
            throw_table_error(DatabaseErrorCode::corrupted_file, file, table.name,
                              "Foreign key reciprocal index is missing or inconsistent");
        }
        const auto& local_index =
            foreign_physical_index(file, table.name, definition, link.index_data_number);
        const auto& target_index = foreign_physical_index(
            file, table.name, target_definition, reciprocal->index_data_number);
        if (local_index.columns.empty() || local_index.columns.size() != target_index.columns.size()) {
            throw_table_error(DatabaseErrorCode::corrupted_file, file, table.name,
                              "Foreign key index column counts are inconsistent");
        }
        const auto target_key = [&](bool old) {
            EncodedIndexKey key;
            for (std::size_t i = 0; i < local_index.columns.size(); ++i) {
                const auto& local_column = foreign_index_column(
                    file, table.name, definition, local_index.columns[i].column_number);
                const auto& indexed = target_index.columns[i];
                const auto target_column = std::find_if(target_definition.columns.begin(),
                                                        target_definition.columns.end(), [&](const auto& column) {
                    return column.column_number == indexed.column_number;
                });
                if (target_column == target_definition.columns.end() || local_column.type != target_column->type) {
                    throw_table_error(DatabaseErrorCode::corrupted_file, file, table.name,
                                      "Foreign key index column types differ");
                }

                const auto value = old ? decode_jet4_value(file, table.name, *old_row, local_column)
                                       : new_values->at(local_column.column_number);
                if (std::holds_alternative<std::monostate>(value)) {
                    ++key.null_count;
                }
                try {
                    const auto part = encode_jet4_index_key(*target_column, value, !indexed.ascending);
                    key.bytes.insert(key.bytes.end(), part.begin(), part.end());
                } catch (const std::invalid_argument& error) {
                    unsupported_table_write(file, table.name, error.what());
                }
            }
            key.bytes = truncate_jet4_index_key(std::move(key.bytes));
            return key;
        };
        const auto key = target_key(parent);
        if (!parent && key.null_count == local_index.columns.size()) {
            continue;
        }
        if (old_row != nullptr && new_values != nullptr &&
            key.bytes == target_key(!parent).bytes) {
            continue;
        }
        IndexEntryCursor cursor(file, target->name, target->definition_page_number,
                                target_index.root_page_number, key.bytes);
        const auto entry = cursor.next_entry();
        const bool found = entry && entry->key == key.bytes;
        if (parent && found) {
            const bool cascades = new_values != nullptr ? (link.cascade_updates_flags & 0x01) != 0
                                                       : (link.cascade_deletes_flags & 0x03) != 0;
            if (cascades) {
                // The dependent rows are rewritten after this row is stored, so the preflight
                // check only has to confirm that the relationship allows it.
                continue;
            }
            throw_table_error(DatabaseErrorCode::constraint_violation, file, table.name,
                              "Foreign key prevents changing a key referenced by dependent rows");
        }
        if (!parent && !found) {
            throw_table_error(DatabaseErrorCode::constraint_violation, file, table.name,
                              "Foreign key has no matching parent row");
        }
    }
}

// Removes the entry that the old row produced and, when new_values is present, inserts the
// entry for the updated row. The entry always points at the logical RowId, so a row that
// moves into an overflow page keeps the same index entry location.
void maintain_index_key_change(const PageFile& file,
                               const ReadableTableSchema& table,
                               const TableDefinition& definition,
                               Jet4IndexPageUpdates& updates,
                               const EncodedTableRow& old_row,
                               RowId row_id,
                               const std::vector<Value>* new_values) {
    std::set<std::uint64_t> freed_pages;
    for (std::size_t physical_index = 0;
         physical_index < definition.physical_indexes.size();
         ++physical_index) {
        const auto& physical = definition.physical_indexes[physical_index];
        const bool primary = is_primary_index(definition, physical_index);
        auto old_key = encode_row_index_key(file, table, definition, physical, old_row.columns, &old_row);
        auto new_key = new_values == nullptr ? EncodedIndexKey{} :
            encode_row_index_key(file, table, definition, physical, *new_values);
        const bool remove_old = !ignores_index_key(physical, old_key);
        const bool insert_new = new_values != nullptr &&
            includes_new_index_key(file, table.name, physical, primary, new_key);
        if ((!remove_old && !insert_new) || (remove_old && insert_new && new_key.bytes == old_key.bytes)) {
            continue;
        }
        const auto row_page = static_cast<std::uint32_t>(row_id.page_number);
        const auto row_number = static_cast<std::uint8_t>(row_id.row_number);
        std::vector<Jet4IndexEntry> removals;
        std::vector<Jet4IndexEntry> insertions;
        if (remove_old) {
            removals.push_back({std::move(old_key.bytes), row_page, row_number});
        }
        if (insert_new) {
            insertions.push_back({std::move(new_key.bytes), row_page, row_number, 0,
                                  new_key.null_count == physical.columns.size()});
        }
        // Update and delete cannot append index pages, so a full leaf is rejected instead of
        // split; insertion through add_row/add_rows does split.
        auto applied = apply_jet4_index_changes(file,
                                                table.name,
                                                table.definition_page_number,
                                                physical.root_page_number,
                                                file.page_count(),
                                                removals,
                                                insertions,
                                                false,
                                                primary || (physical.flags & index_unique_flag) != 0);
        for (auto& page : applied.pages) {
            updates.pages[page.page_number] = std::move(page.bytes);
        }
        updates.count_deltas[physical_index] += applied.distinct_key_delta;
        if (!applied.freed_pages.empty()) {
            // The merged-away leaf leaves the index owned-page map and becomes free again in the
            // global map, so later allocations (by us or by Access) can reuse it.
            const std::set<std::uint64_t> freed(applied.freed_pages.begin(),
                                                applied.freed_pages.end());
            update_usage_map_row(file,
                                 table.name,
                                 updates.pages,
                                 physical.owned_pages,
                                 {},
                                 freed,
                                 false);
            freed_pages.insert(freed.begin(), freed.end());
        }
    }

    if (!freed_pages.empty()) {
        // Page 1 is read once here: applying two separate releases would drop the first one.
        mark_global_pages_free(
            file, updates.pages, std::vector<std::uint64_t>(freed_pages.begin(), freed_pages.end()));
    }
}

// Applies the distinct-key count changes to the 12-byte blocks in the table definition.
void apply_index_count_deltas(const PageFile& file,
                              const std::string& table_name,
                              std::uint64_t definition_page_number,
                              const TableDefinition& definition,
                              const Jet4IndexPageUpdates& updates,
                              std::map<std::uint64_t, std::vector<std::uint8_t>>& pages) {
    if (std::none_of(updates.count_deltas.begin(),
                     updates.count_deltas.end(),
                     [](const auto& entry) { return entry.second != 0; })) {
        return;
    }
    auto& definition_page = mutable_page(pages, file, definition_page_number);
    for (const auto& entry : updates.count_deltas) {
        if (entry.second == 0 || entry.first >= definition.physical_indexes.size()) {
            continue;
        }
        const std::int64_t next =
            static_cast<std::int64_t>(definition.physical_indexes[entry.first].unique_entry_count) +
            entry.second;
        if (next < 0) {
            throw_table_error(DatabaseErrorCode::corrupted_file,
                              file,
                              table_name,
                              "Index entry count would become negative",
                              definition_page_number);
        }
        write_u32(definition_page,
                  63 + entry.first * 12 + 4,
                  static_cast<std::uint32_t>(next));
    }
}

void update_long_value_usage_maps(const PageFile& file,
                                  const std::string& table_name,
                                  const TableDefinition& definition,
                                  const std::map<std::uint16_t, std::set<std::uint64_t>>& pages_by_column,
                                  const std::map<std::uint16_t, std::set<std::uint64_t>>& full_pages_by_column,
                                  std::map<std::uint64_t, std::vector<std::uint8_t>>& updated_pages,
                                  UsageBitmapAllocation& allocation) {
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
        update_usage_map_row(file,
                             table_name,
                             updated_pages,
                             *column->long_value_owned_pages,
                             entry.second,
                             {},
                             false,
                             &allocation);
        // Reused pages stay owned, but a page that no longer holds free space leaves the
        // column free-space map so later inserts do not pick it again.
        const auto full_entry = full_pages_by_column.find(entry.first);
        const std::set<std::uint64_t> no_pages;
        const std::set<std::uint64_t>& full_pages =
            full_entry == full_pages_by_column.end() ? no_pages : full_entry->second;
        std::set<std::uint64_t> free_pages;
        std::set_difference(entry.second.begin(),
                            entry.second.end(),
                            full_pages.begin(),
                            full_pages.end(),
                            std::inserter(free_pages, free_pages.end()));
        update_usage_map_row(file,
                             table_name,
                             updated_pages,
                             *column->long_value_free_space_pages,
                             free_pages,
                             full_pages,
                             false,
                             &allocation);
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

namespace {

std::string_view trim_expression(std::string_view text) {
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = text.find_first_not_of(whitespace);
    return first == std::string_view::npos
        ? std::string_view{} : text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

std::optional<double> parse_floating_validation_token(std::string_view& text) {
    text = trim_expression(text);
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
        if (text.empty() || text.front() == '+' || text.front() == '-') {
            return std::nullopt;
        }
    }
    if (text.empty()) {
        return std::nullopt;
    }
    double value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                        value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr == text.data() || !std::isfinite(value)) {
        return std::nullopt;
    }
    text.remove_prefix(static_cast<std::size_t>(parsed.ptr - text.data()));
    return value;
}

struct DecimalValidationNumber {
    bool negative;
    std::string digits;
    std::int64_t scale;
};

std::optional<DecimalValidationNumber> parse_decimal_validation_token(std::string_view& text) {
    text = trim_expression(text);
    if (text.empty()) {
        return std::nullopt;
    }

    std::size_t position = 0;
    bool negative = false;
    if (text[position] == '+' || text[position] == '-') {
        negative = text[position] == '-';
        if (++position == text.size()) {
            return std::nullopt;
        }
    }

    std::string digits;
    digits.reserve(text.size());
    std::size_t fraction_digits = 0;
    bool decimal_point_seen = false;
    bool digit_seen = false;
    for (; position < text.size() && text[position] != 'e' && text[position] != 'E'; ++position) {
        const char character = text[position];
        if (character == '.') {
            if (decimal_point_seen) {
                return std::nullopt;
            }
            decimal_point_seen = true;
            continue;
        }
        if (character < '0' || character > '9') {
            break;
        }
        digits.push_back(character);
        digit_seen = true;
        if (decimal_point_seen) {
            ++fraction_digits;
        }
    }
    if (!digit_seen) {
        return std::nullopt;
    }

    std::int64_t exponent = 0;
    if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        bool negative_exponent = false;
        if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
            negative_exponent = text[position] == '-';
            ++position;
        }
        const auto exponent_start = position;
        constexpr std::int64_t maximum_exponent = std::numeric_limits<std::int32_t>::max();
        for (; position < text.size() && text[position] >= '0' && text[position] <= '9'; ++position) {
            const auto digit = static_cast<std::int64_t>(text[position] - '0');
            if (exponent > (maximum_exponent - digit) / 10) {
                return std::nullopt;
            }
            exponent = exponent * 10 + digit;
        }
        if (position == exponent_start) {
            return std::nullopt;
        }
        if (negative_exponent) {
            exponent = -exponent;
        }
    }

    if (fraction_digits > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return std::nullopt;
    }
    const std::int64_t scale = static_cast<std::int64_t>(fraction_digits) - exponent;
    if (scale < std::numeric_limits<std::int32_t>::min() ||
        scale > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }

    const auto first_significant = digits.find_first_not_of('0');
    if (first_significant == std::string::npos) {
        text.remove_prefix(position);
        digits = "0";
        negative = false;
        return DecimalValidationNumber{negative, std::move(digits), 0};
    }
    digits.erase(0, first_significant);
    text.remove_prefix(position);
    return DecimalValidationNumber{negative, std::move(digits), scale};
}

int compare_decimal_validation_numbers(const DecimalValidationNumber& left,
                                       const DecimalValidationNumber& right) {
    const bool left_zero = left.digits == "0";
    const bool right_zero = right.digits == "0";
    if (left_zero || right_zero) {
        if (left_zero && right_zero) return 0;
        if (left_zero) return right.negative ? 1 : -1;
        return left.negative ? -1 : 1;
    }
    if (left.negative != right.negative) {
        return left.negative ? -1 : 1;
    }

    const std::int64_t left_order = static_cast<std::int64_t>(left.digits.size()) - left.scale;
    const std::int64_t right_order = static_cast<std::int64_t>(right.digits.size()) - right.scale;
    int magnitude_order = 0;
    if (left_order != right_order) {
        magnitude_order = left_order < right_order ? -1 : 1;
    } else {
        const auto digit_count = std::max(left.digits.size(), right.digits.size());
        for (std::size_t index = 0; index < digit_count; ++index) {
            const char left_digit = index < left.digits.size() ? left.digits[index] : '0';
            const char right_digit = index < right.digits.size() ? right.digits[index] : '0';
            if (left_digit != right_digit) {
                magnitude_order = left_digit < right_digit ? -1 : 1;
                break;
            }
        }
    }
    return left.negative ? -magnitude_order : magnitude_order;
}

std::optional<DecimalValidationNumber> decimal_value_for_validation(const Value& value) {
    const auto* decimal = std::get_if<DecimalValue>(&value);
    if (decimal == nullptr) {
        return std::nullopt;
    }
    std::string_view text(decimal->text);
    auto parsed = parse_decimal_validation_token(text);
    if (!parsed || !trim_expression(text).empty()) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<std::string> date_only_default_to_iso8601(std::string_view expression) {
    if (expression.size() < 8 || expression.front() != '#' || expression.back() != '#') {
        return std::nullopt;
    }
    const auto literal = expression.substr(1, expression.size() - 2);
    const auto first_separator = literal.find('/');
    const auto second_separator = first_separator == std::string_view::npos
                                      ? std::string_view::npos
                                      : literal.find('/', first_separator + 1);
    if (first_separator == std::string_view::npos || second_separator == std::string_view::npos ||
        literal.find('/', second_separator + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto month_text = literal.substr(0, first_separator);
    const auto day_text = literal.substr(first_separator + 1, second_separator - first_separator - 1);
    const auto year_text = literal.substr(second_separator + 1);
    const auto parse_component = [](std::string_view text, int& value) {
        if (text.empty()) return false;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    };
    int month = 0;
    int day = 0;
    int year = 0;
    if (month_text.size() > 2 || day_text.size() > 2 || year_text.size() != 4 ||
        !parse_component(month_text, month) || !parse_component(day_text, day) ||
        !parse_component(year_text, year)) {
        return std::nullopt;
    }
    static constexpr int days_per_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 100 || month < 1 || month > 12) {
        return std::nullopt;
    }
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int last_day = month == 2 && leap ? 29 : days_per_month[month];
    if (day < 1 || day > last_day) {
        return std::nullopt;
    }
    const auto padded_month = month < 10 ? "0" + std::to_string(month) : std::to_string(month);
    const auto padded_day = day < 10 ? "0" + std::to_string(day) : std::to_string(day);
    return std::string(year_text) + "-" + padded_month + "-" + padded_day + "T00:00";
}

Value evaluate_column_default(const PageFile& file, const ReadableTableSchema& table,
                              const ColumnDefinition& column) {
    auto expression = trim_expression(*column.default_expression);
    if (!expression.empty() && expression.front() == '=') {
        expression = trim_expression(expression.substr(1));
    }
    if (expression.empty()) {
        return {};
    }
    std::string keyword(expression);
    for (auto& c : keyword) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    if (keyword == "null") {
        return {};
    }
    if (column.type == 0x01) {
        if (keyword == "true" || keyword == "yes" || keyword == "-1") return true;
        if (keyword == "false" || keyword == "no" || keyword == "0") return false;
    }
    if ((column.type == 0x0a || column.type == 0x0c) && expression.size() >= 2 &&
        (expression.front() == '"' || expression.front() == char{39}) &&
        expression.back() == expression.front()) {
        std::string text;
        for (std::size_t i = 1; i + 1 < expression.size(); ++i) {
            if (expression[i] == expression.front()) {
                ++i;
                if (i + 1 >= expression.size() || expression[i] != expression.front()) {
                    unsupported_column_write(file, table, column, "Unsupported default expression");
                }
            }
            text.push_back(expression[i]);
        }
        if (column.type == 0x0c) {
            return MemoValue{std::move(text), {}};
        }
        return text;
    }

    const auto numeric_literal = expression.front() == '+' ? expression.substr(1) : expression;
    if (column.type == 0x06) {
        float number{};
        if (!numeric_literal.empty()) {
            const auto parsed = std::from_chars(numeric_literal.data(),
                                                numeric_literal.data() + numeric_literal.size(),
                                                number,
                                                std::chars_format::general);
            if (parsed.ec == std::errc{} && parsed.ptr == numeric_literal.data() + numeric_literal.size() &&
                std::isfinite(number)) {
                return number;
            }
        }
        unsupported_column_write(file, table, column, "Unsupported floating-point default expression");
    }
    if (column.type == 0x07) {
        double number{};
        if (!numeric_literal.empty()) {
            const auto parsed = std::from_chars(numeric_literal.data(),
                                                numeric_literal.data() + numeric_literal.size(),
                                                number,
                                                std::chars_format::general);
            if (parsed.ec == std::errc{} && parsed.ptr == numeric_literal.data() + numeric_literal.size() &&
                std::isfinite(number)) {
                return number;
            }
        }
        unsupported_column_write(file, table, column, "Unsupported floating-point default expression");
    }
    if (column.type == 0x05) {
        try {
            (void)jet4_money_scaled_value(numeric_literal);
            return DecimalValue{std::string(numeric_literal)};
        } catch (const std::invalid_argument&) {
            unsupported_column_write(file, table, column, "Unsupported MONEY default expression");
        }
    }
    if (column.type == 0x08) {
        if (const auto date = date_only_default_to_iso8601(expression)) {
            return DateTimeValue{*date};
        }
        unsupported_column_write(file, table, column,
                                 "Unsupported SHORT_DATE_TIME default expression");
    }

    const auto digits = numeric_literal;
    std::int32_t number{};
    if (!digits.empty()) {
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size()) {
            if (column.type == 0x04) return number;
            if (column.type == 0x03 && number >= INT16_MIN && number <= INT16_MAX) {
                return static_cast<std::int16_t>(number);
            }
            if (column.type == 0x02 && number >= 0 && number <= UINT8_MAX) {
                return static_cast<std::int8_t>(static_cast<std::uint8_t>(number));
            }
        }
    }
    unsupported_column_write(file, table, column, "Unsupported default expression");
}

bool has_column_validators(const Jet4WriteContext& context, std::size_t table_index) {
    if (!context.column_validators || table_index >= context.column_validators->size()) {
        return false;
    }
    const auto& validators = context.column_validators->at(table_index);
    return std::any_of(validators.begin(), validators.end(), [](const auto& validator) {
        return static_cast<bool>(validator);
    });
}

void apply_column_validators(const Jet4WriteContext& context,
                             std::size_t table_index,
                             std::vector<Value>& values) {
    if (!has_column_validators(context, table_index)) {
        return;
    }
    if (!context.columns || table_index >= context.columns->size()) {
        throw std::logic_error("Column validator metadata is unavailable");
    }
    const auto& columns = context.columns->at(table_index);
    const auto& validators = context.column_validators->at(table_index);
    if (values.size() != columns.size() || validators.size() != columns.size()) {
        throw std::invalid_argument("Row value count does not match the table column count");
    }
    for (std::size_t i = 0; i < validators.size(); ++i) {
        if (validators[i]) {
            values[i] = validators[i](columns[i], values[i]);
        }
    }
}

std::optional<std::int64_t> integer_value_for_validation(const Value& value) {
    if (const auto* byte = std::get_if<std::int8_t>(&value)) {
        return static_cast<std::uint8_t>(*byte);
    }
    if (const auto* small = std::get_if<std::int16_t>(&value)) {
        return *small;
    }
    if (const auto* integer = std::get_if<std::int32_t>(&value)) {
        return *integer;
    }
    return std::nullopt;
}

std::optional<double> floating_value_for_validation(const Value& value) {
    if (const auto* single = std::get_if<float>(&value)) {
        return static_cast<double>(*single);
    }
    if (const auto* number = std::get_if<double>(&value)) {
        return *number;
    }
    return std::nullopt;
}

bool ascii_equal_ignore_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    return std::equal(left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b) {
        const auto fold = [](unsigned char value) {
            return value >= 'A' && value <= 'Z'
                       ? static_cast<unsigned char>(value - 'A' + 'a')
                       : value;
        };
        return fold(a) == fold(b);
    });
}

class TableValidationParser {
public:
    TableValidationParser(const PageFile& file,
                          const ReadableTableSchema& table,
                          const TableDefinition& definition,
                          const std::vector<Value>& row,
                          std::string_view expression)
        : file_(file), table_(table), definition_(definition), row_(row), expression_(expression) {}

    bool evaluate() {
        const bool accepted = parse_or_expression();
        skip_whitespace();
        if (position_ != expression_.size()) {
            unsupported_table_write(file_, table_.name, "Unsupported table validation rule expression");
        }
        return accepted;
    }

private:
    void skip_whitespace() {
        while (position_ < expression_.size()) {
            const char value = expression_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                break;
            }
            ++position_;
        }
    }

    bool consume(char value) {
        skip_whitespace();
        if (position_ == expression_.size() || expression_[position_] != value) {
            return false;
        }
        ++position_;
        return true;
    }

    bool consume_keyword(std::string_view keyword) {
        skip_whitespace();
        if (expression_.size() - position_ < keyword.size()) {
            return false;
        }
        if (position_ > 0) {
            const auto previous = static_cast<unsigned char>(expression_[position_ - 1]);
            if ((previous >= 'A' && previous <= 'Z') || (previous >= 'a' && previous <= 'z') ||
                (previous >= '0' && previous <= '9') || previous == '_') {
                return false;
            }
        }
        for (std::size_t i = 0; i < keyword.size(); ++i) {
            const auto value = static_cast<unsigned char>(expression_[position_ + i]);
            const auto folded = value >= 'A' && value <= 'Z'
                                    ? static_cast<unsigned char>(value - 'A' + 'a')
                                    : value;
            if (folded != static_cast<unsigned char>(keyword[i])) {
                return false;
            }
        }
        const auto end = position_ + keyword.size();
        if (end < expression_.size()) {
            const auto next = static_cast<unsigned char>(expression_[end]);
            if ((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') ||
                (next >= '0' && next <= '9') || next == '_') {
                return false;
            }
        }
        position_ = end;
        return true;
    }

    bool consume_and() {
        return consume_keyword("and");
    }

    bool consume_or() {
        return consume_keyword("or");
    }

    bool parse_and_expression() {
        bool accepted = parse_primary();
        while (consume_and()) {
            const bool right = parse_primary();
            accepted = accepted && right;
        }
        return accepted;
    }

    bool parse_or_expression() {
        bool accepted = parse_and_expression();
        while (consume_or()) {
            const bool right = parse_and_expression();
            accepted = accepted || right;
        }
        return accepted;
    }

    bool parse_primary() {
        skip_whitespace();
        if (consume('(')) {
            const bool accepted = parse_or_expression();
            if (!consume(')')) {
                unsupported_table_write(file_, table_.name,
                                        "Unclosed parenthesis in table validation rule");
            }
            return accepted;
        }
        return parse_comparison();
    }

    std::int64_t parse_integer_literal() {
        skip_whitespace();
        if (position_ == expression_.size()) {
            unsupported_table_write(file_, table_.name,
                                    "Expected an integer operand in table validation rule");
        }
        if (expression_[position_] == '+') {
            ++position_;
            if (position_ == expression_.size() || expression_[position_] == '+' ||
                expression_[position_] == '-') {
                unsupported_table_write(file_, table_.name,
                                        "Expected an integer operand in table validation rule");
            }
        }
        std::int64_t value{};
        const char* begin = expression_.data() + position_;
        const char* end = expression_.data() + expression_.size();
        const auto parsed = std::from_chars(begin, end, value);
        if (parsed.ec != std::errc{} || parsed.ptr == begin) {
            unsupported_table_write(file_, table_.name,
                                    "Expected an integer operand in table validation rule");
        }
        position_ = static_cast<std::size_t>(parsed.ptr - expression_.data());
        return value;
    }

    double parse_floating_point_literal() {
        skip_whitespace();
        auto remaining = expression_.substr(position_);
        const auto value = parse_floating_validation_token(remaining);
        if (!value) {
            unsupported_table_write(file_, table_.name,
                                    "Expected a finite numeric operand in table validation rule");
        }
        position_ = expression_.size() - remaining.size();
        return *value;
    }

    DecimalValidationNumber parse_decimal_literal() {
        skip_whitespace();
        auto remaining = expression_.substr(position_);
        auto value = parse_decimal_validation_token(remaining);
        if (!value) {
            unsupported_table_write(file_, table_.name,
                                    "Expected a finite decimal operand in table validation rule");
        }
        position_ = expression_.size() - remaining.size();
        return std::move(*value);
    }

    bool parse_comparison() {
        if (!consume('[')) {
            unsupported_table_write(file_, table_.name,
                                    "Table validation comparisons must reference named columns");
        }
        const auto name_start = position_;
        while (position_ < expression_.size() && expression_[position_] != ']') {
            ++position_;
        }
        if (position_ == expression_.size() || position_ == name_start) {
            unsupported_table_write(file_, table_.name,
                                    "Invalid column reference in table validation rule");
        }
        const auto name = expression_.substr(name_start, position_ - name_start);
        ++position_;
        const auto column = std::find_if(definition_.columns.begin(), definition_.columns.end(),
                                         [&](const auto& item) {
                                             return ascii_equal_ignore_case(item.name, name);
                                         });
        if (column == definition_.columns.end()) {
            unsupported_table_write(file_, table_.name,
                                    "Table validation references an unknown column");
        }
        const auto column_index = static_cast<std::size_t>(std::distance(definition_.columns.begin(), column));

        skip_whitespace();
        if (consume_keyword("is")) {
            const bool is_not = consume_keyword("not");
            if (!consume_keyword("null")) {
                unsupported_table_write(file_, table_.name,
                                        "Expected NULL after IS in table validation rule");
            }
            if (column_index >= row_.size()) {
                unsupported_table_write(file_, table_.name,
                                        "Table validation row is missing a referenced column");
            }
            const bool null_value = std::holds_alternative<std::monostate>(row_[column_index]);
            return null_value != is_not;
        }
        const bool integer_column = column->type >= 0x02 && column->type <= 0x04;
        const bool floating_column = column->type == 0x06 || column->type == 0x07;
        const bool money_column = column->type == 0x05;
        if (!integer_column && !floating_column && !money_column) {
            unsupported_table_write(file_, table_.name,
                                    "Table validation comparisons require supported numeric columns");
        }

        if (consume_keyword("between")) {
            if (integer_column) {
                auto lower = parse_integer_literal();
                if (!consume_keyword("and")) {
                    unsupported_table_write(file_, table_.name,
                                            "Expected AND between table validation bounds");
                }
                auto upper = parse_integer_literal();
                if (lower > upper) std::swap(lower, upper);
                const auto value = column_index < row_.size()
                                       ? integer_value_for_validation(row_[column_index])
                                       : std::nullopt;
                return value ? *value >= lower && *value <= upper : false;
            }
            if (floating_column) {
                auto lower = parse_floating_point_literal();
                if (!consume_keyword("and")) {
                    unsupported_table_write(file_, table_.name,
                                            "Expected AND between table validation bounds");
                }
                auto upper = parse_floating_point_literal();
                if (lower > upper) std::swap(lower, upper);
                const auto value = column_index < row_.size()
                                       ? floating_value_for_validation(row_[column_index])
                                       : std::nullopt;
                return value ? *value >= lower && *value <= upper : false;
            }
            auto lower = parse_decimal_literal();
            if (!consume_keyword("and")) {
                unsupported_table_write(file_, table_.name,
                                        "Expected AND between table validation bounds");
            }
            auto upper = parse_decimal_literal();
            if (compare_decimal_validation_numbers(lower, upper) > 0) std::swap(lower, upper);
            const auto value = column_index < row_.size()
                                   ? decimal_value_for_validation(row_[column_index])
                                   : std::nullopt;
            if (!value) return false;
            return compare_decimal_validation_numbers(*value, lower) >= 0 &&
                   compare_decimal_validation_numbers(*value, upper) <= 0;
        }

        std::string_view op;
        for (const auto candidate : {std::string_view{"<="}, std::string_view{">="},
                                     std::string_view{"<>"}, std::string_view{"="},
                                     std::string_view{"<"}, std::string_view{">"}}) {
            if (expression_.substr(position_, candidate.size()) == candidate) {
                op = candidate;
                position_ += candidate.size();
                break;
            }
        }
        if (op.empty()) {
            unsupported_table_write(file_, table_.name,
                                    "Unsupported comparison in table validation rule");
        }
        const auto compare = [&](auto value, auto limit) {
            if (op == "<=") return value <= limit;
            if (op == ">=") return value >= limit;
            if (op == "<>") return value != limit;
            if (op == "=") return value == limit;
            if (op == "<") return value < limit;
            return value > limit;
        };
        if (integer_column) {
            const auto limit = parse_integer_literal();
            const auto value = column_index < row_.size()
                                   ? integer_value_for_validation(row_[column_index])
                                   : std::nullopt;
            return value ? compare(*value, limit) : false;
        }
        if (floating_column) {
            const auto limit = parse_floating_point_literal();
            const auto value = column_index < row_.size()
                                   ? floating_value_for_validation(row_[column_index])
                                   : std::nullopt;
            return value ? compare(*value, limit) : false;
        }
        const auto limit = parse_decimal_literal();
        const auto value = column_index < row_.size()
                               ? decimal_value_for_validation(row_[column_index])
                               : std::nullopt;
        if (!value) return false;
        const int order = compare_decimal_validation_numbers(*value, limit);
        if (op == "<=") return order <= 0;
        if (op == ">=") return order >= 0;
        if (op == "<>") return order != 0;
        if (op == "=") return order == 0;
        if (op == "<") return order < 0;
        return order > 0;
    }

    const PageFile& file_;
    const ReadableTableSchema& table_;
    const TableDefinition& definition_;
    const std::vector<Value>& row_;
    std::string_view expression_;
    std::size_t position_ = 0;
};

void validate_column_rules(const PageFile& file, const ReadableTableSchema& table,
                           const TableDefinition& definition, const std::vector<Value>& row) {
    for (std::size_t i = 0; i < row.size(); ++i) {
        const auto& column = definition.columns.at(i);
        if (!column.validation_rule) {
            continue;
        }
        auto rule = trim_expression(*column.validation_rule);
        if (rule.empty()) {
            continue;
        }
        if (rule.front() == '[') {
            const auto end = rule.find(']');
            const auto identifier = end == std::string_view::npos ? std::string_view{} : rule.substr(1, end - 1);
            if (end == std::string_view::npos ||
                !ascii_equal_ignore_case(identifier, column.name)) {
                unsupported_column_write(file, table, column, "Validation rule must reference the current column");
            }
            rule = trim_expression(rule.substr(end + 1));
        }
        const bool null_value = std::holds_alternative<std::monostate>(row[i]);
        std::string keyword(rule);
        for (auto& c : keyword) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        const auto parse_integer_token = [](std::string_view& text) -> std::optional<std::int64_t> {
            text = trim_expression(text);
            if (!text.empty() && text.front() == '+') {
                text.remove_prefix(1);
                if (text.empty() || text.front() == '+' || text.front() == '-') {
                    return std::nullopt;
                }
            }
            if (text.empty()) {
                return std::nullopt;
            }
            std::int64_t value{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr == text.data()) {
                return std::nullopt;
            }
            text.remove_prefix(static_cast<std::size_t>(parsed.ptr - text.data()));
            return value;
        };
        const auto integer_value = [&]() { return integer_value_for_validation(row[i]); };
        const bool integer_column = column.type >= 0x02 && column.type <= 0x04;
        const bool floating_column = column.type == 0x06 || column.type == 0x07;
        const bool money_column = column.type == 0x05;
        bool accepted = false;
        if (keyword == "is null" || keyword == "is not null") {
            accepted = null_value == (keyword == "is null");
        } else if (keyword.rfind("between", 0) == 0 && keyword.size() > 7 &&
                   (keyword[7] == ' ' || keyword[7] == '\t' || keyword[7] == '\r' || keyword[7] == '\n')) {
            if (!integer_column && !floating_column && !money_column) {
                unsupported_column_write(file, table, column,
                                         "BETWEEN validation requires a supported numeric column");
            }
            std::string_view bounds(keyword);
            bounds.remove_prefix(7);
            if (integer_column) {
                const auto lower = parse_integer_token(bounds);
                bounds = trim_expression(bounds);
                if (!lower || bounds.size() < 4 || bounds.substr(0, 3) != "and" ||
                    !(bounds[3] == ' ' || bounds[3] == '\t' || bounds[3] == '\r' || bounds[3] == '\n')) {
                    unsupported_column_write(file, table, column,
                                             "Expected integer bounds in BETWEEN validation rule");
                }
                bounds.remove_prefix(3);
                const auto upper = parse_integer_token(bounds);
                if (!upper || !trim_expression(bounds).empty()) {
                    unsupported_column_write(file, table, column,
                                             "Expected integer bounds in BETWEEN validation rule");
                }
                auto ordered_lower = *lower;
                auto ordered_upper = *upper;
                if (ordered_lower > ordered_upper) std::swap(ordered_lower, ordered_upper);
                if (const auto value = integer_value()) {
                    accepted = *value >= ordered_lower && *value <= ordered_upper;
                }
            } else if (floating_column) {
                const auto lower = parse_floating_validation_token(bounds);
                bounds = trim_expression(bounds);
                if (!lower || bounds.size() < 4 || bounds.substr(0, 3) != "and" ||
                    !(bounds[3] == ' ' || bounds[3] == '\t' || bounds[3] == '\r' || bounds[3] == '\n')) {
                    unsupported_column_write(file, table, column,
                                             "Expected finite numeric bounds in BETWEEN validation rule");
                }
                bounds.remove_prefix(3);
                const auto upper = parse_floating_validation_token(bounds);
                if (!upper || !trim_expression(bounds).empty()) {
                    unsupported_column_write(file, table, column,
                                             "Expected finite numeric bounds in BETWEEN validation rule");
                }
                auto ordered_lower = *lower;
                auto ordered_upper = *upper;
                if (ordered_lower > ordered_upper) std::swap(ordered_lower, ordered_upper);
                if (const auto value = floating_value_for_validation(row[i])) {
                    accepted = *value >= ordered_lower && *value <= ordered_upper;
                }
            } else {
                const auto lower = parse_decimal_validation_token(bounds);
                bounds = trim_expression(bounds);
                if (!lower || bounds.size() < 4 || bounds.substr(0, 3) != "and" ||
                    !(bounds[3] == ' ' || bounds[3] == '\t' || bounds[3] == '\r' || bounds[3] == '\n')) {
                    unsupported_column_write(file, table, column,
                                             "Expected finite decimal bounds in BETWEEN validation rule");
                }
                bounds.remove_prefix(3);
                const auto upper = parse_decimal_validation_token(bounds);
                if (!upper || !trim_expression(bounds).empty()) {
                    unsupported_column_write(file, table, column,
                                             "Expected finite decimal bounds in BETWEEN validation rule");
                }
                auto ordered_lower = *lower;
                auto ordered_upper = *upper;
                if (compare_decimal_validation_numbers(ordered_lower, ordered_upper) > 0) {
                    std::swap(ordered_lower, ordered_upper);
                }
                if (const auto value = decimal_value_for_validation(row[i])) {
                    accepted = compare_decimal_validation_numbers(*value, ordered_lower) >= 0 &&
                               compare_decimal_validation_numbers(*value, ordered_upper) <= 0;
                }
            }
        } else {
            constexpr std::string_view operators[]{"<=", ">=", "<>", "=", "<", ">"};
            const auto comparison = std::find_if(std::begin(operators), std::end(operators), [&](auto op) {
                return rule.substr(0, op.size()) == op;
            });
            if (comparison == std::end(operators) ||
                (!integer_column && !floating_column && !money_column)) {
                unsupported_column_write(file, table, column, "Unsupported column validation rule");
            }
            auto literal = rule.substr(comparison->size());
            const auto compare = [&](auto value, auto limit) {
                if (*comparison == "<=") return value <= limit;
                if (*comparison == ">=") return value >= limit;
                if (*comparison == "<>") return value != limit;
                if (*comparison == "=") return value == limit;
                if (*comparison == "<") return value < limit;
                return value > limit;
            };
            if (integer_column) {
                const auto limit = parse_integer_token(literal);
                if (!limit || !trim_expression(literal).empty()) {
                    unsupported_column_write(file, table, column, "Expected an integer validation operand");
                }
                if (const auto value = integer_value()) {
                    accepted = compare(*value, *limit);
                }
            } else if (floating_column) {
                const auto limit = parse_floating_validation_token(literal);
                if (!limit || !trim_expression(literal).empty()) {
                    unsupported_column_write(file, table, column,
                                             "Expected a finite numeric validation operand");
                }
                if (const auto value = floating_value_for_validation(row[i])) {
                    accepted = compare(*value, *limit);
                }
            } else {
                const auto limit = parse_decimal_validation_token(literal);
                if (!limit || !trim_expression(literal).empty()) {
                    unsupported_column_write(file, table, column,
                                             "Expected a finite decimal validation operand");
                }
                if (const auto value = decimal_value_for_validation(row[i])) {
                    const int order = compare_decimal_validation_numbers(*value, *limit);
                    if (*comparison == "<=") accepted = order <= 0;
                    if (*comparison == ">=") accepted = order >= 0;
                    if (*comparison == "<>") accepted = order != 0;
                    if (*comparison == "=") accepted = order == 0;
                    if (*comparison == "<") accepted = order < 0;
                    if (*comparison == ">") accepted = order > 0;
                }
            }
        }
        if (!accepted) {
            DatabaseErrorContext error;
            error.table_name = table.name;
            error.column_name = column.name;
            error.column_number = column.column_number;
            const auto message = column.validation_text ? trim_expression(*column.validation_text) : std::string_view{};
            throw DatabaseError(DatabaseErrorCode::constraint_violation,
                                message.empty() ? "Column validation rule failed" : std::string(message),
                                file.path(), table.definition_page_number, std::move(error));
        }
    }
}

void validate_table_rule(const PageFile& file, const ReadableTableSchema& table,
                         const TableDefinition& definition, const std::vector<Value>& row) {
    if (!definition.table_validation_rule) {
        return;
    }
    const auto rule = trim_expression(*definition.table_validation_rule);
    if (rule.empty()) {
        return;
    }
    if (!TableValidationParser(file, table, definition, row, rule).evaluate()) {
        DatabaseErrorContext error;
        error.table_name = table.name;
        const auto message = definition.table_validation_text
                                 ? trim_expression(*definition.table_validation_text)
                                 : std::string_view{};
        throw DatabaseError(DatabaseErrorCode::constraint_violation,
                            message.empty() ? "Table validation rule failed" : std::string(message),
                            file.path(), table.definition_page_number, std::move(error));
    }
}

std::vector<RowId> append_rows_impl(Jet4WriteContext& context,
                                   std::size_t table_index,
                                   const std::vector<std::vector<Value>>& input_rows,
                                   bool allow_catalog_table) {
    if (input_rows.empty()) {
        return {};
    }
    if (!context.writable) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Database was opened read-only; reopen it in read-write mode to insert rows",
                            context.file.path());
    }
    std::lock_guard<std::recursive_mutex> write_lock(context.write_mutex);
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
        validate_local_table(file, table, definition, allow_catalog_table);
        if (definition.owned_pages.page_number == 0 || definition.free_space_pages.page_number == 0 ||
            definition.owned_pages.page_number != definition.free_space_pages.page_number) {
            unsupported_table_write(file,
                                    table.name,
                                    "Row insertion currently requires the table usage maps to share one declaration page");
        }
        const bool has_defaults = !allow_catalog_table &&
            std::any_of(definition.columns.begin(), definition.columns.end(),
                        [](const auto& column) { return column.default_expression.has_value(); });
        const bool has_validators = !allow_catalog_table && has_column_validators(context, table_index);
        std::optional<std::vector<std::vector<Value>>> resolved_rows;
        if (has_defaults || has_validators) {
            resolved_rows = input_rows;
            for (auto& row : *resolved_rows) {
                if (row.size() > definition.columns.size()) {
                    throw std::invalid_argument("Row value count exceeds table column count");
                }
                row.resize(definition.columns.size());
                if (has_defaults) {
                    for (std::size_t i = 0; i < row.size(); ++i) {
                        const auto& column = definition.columns[i];
                        if (column.default_expression && std::holds_alternative<std::monostate>(row[i])) {
                            row[i] = evaluate_column_default(file, table, column);
                        }
                    }
                }
                if (has_validators) {
                    apply_column_validators(context, table_index, row);
                }
            }
        }
        const auto& rows = resolved_rows ? *resolved_rows : input_rows;
        if (rows.size() > std::numeric_limits<std::uint32_t>::max() - definition.row_count) {
            unsupported_table_write(file, table.name, "Table row count would exceed Jet4 limits");
        }

        const bool catalog_object = allow_catalog_table && table.definition_page_number == 2;
        const bool access_catalog = allow_catalog_table && table.name == "MSysACEs";
        const auto fixed_data_size = fixed_row_data_size(file, table, definition,
                                                         catalog_object || access_catalog);
        std::vector<EncodedRow> encoded_rows;
        encoded_rows.reserve(rows.size());
        for (const auto& row : rows) {
            encoded_rows.push_back(encode_row(file, table, definition, row, fixed_data_size,
                                              nullptr, nullptr, catalog_object));
            validate_column_rules(file, table, definition, row);
            validate_table_rule(file, table, definition, row);
            validate_foreign_keys(file, context.schema, table, definition, nullptr, &row);
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
                                                                      definition,
                                                                      encoded_rows,
                                                                      file.page_count());
        if (catalog_object) {
            // MSysObjects.Database may use legacy LVAL storage without a column usage map.
            for (const auto& column : definition.columns) {
                if (column.name == "Database" && !column.long_value_owned_pages &&
                    !column.long_value_free_space_pages) {
                    long_value_pages.pages_by_column.erase(column.column_number);
                }
            }
        }
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
        }
        appended_page_numbers.insert(appended_page_numbers.end(),
                                     long_value_pages.appended_pages.begin(),
                                     long_value_pages.appended_pages.end());
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

        std::map<std::uint64_t, std::vector<std::uint8_t>> index_pages;
        std::vector<std::vector<std::uint64_t>> appended_pages_by_index(
            definition.physical_indexes.size());
        std::vector<std::uint64_t> appended_index_pages;
        std::vector<std::int64_t> index_entry_increments(definition.physical_indexes.size(), 0);
        // Index pages are appended after the long-value and data pages, so the allocator starts
        // at the first page number the data pages did not take.
        std::uint64_t next_index_page_number = next_page_number;
        for (std::size_t physical_index = 0;
             physical_index < definition.physical_indexes.size();
             ++physical_index) {
            const auto& physical = definition.physical_indexes[physical_index];
            const bool primary = is_primary_index(definition, physical_index);
            // One call per index keeps a single page view for the whole batch; applying keys one
            // by one would re-read the same leaf from disk and drop the earlier entries.
            std::vector<Jet4IndexEntry> entries;
            entries.reserve(rows.size());
            for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
                auto key = encode_row_index_key(file, table, definition, physical, rows[row_index]);
                if (!includes_new_index_key(file, table.name, physical, primary, key)) {
                    continue;
                }
                entries.push_back({std::move(key.bytes),
                                   static_cast<std::uint32_t>(row_ids[row_index].page_number),
                                   static_cast<std::uint8_t>(row_ids[row_index].row_number),
                                   0, key.null_count == physical.columns.size()});
            }
            auto applied = apply_jet4_index_changes(file,
                                                    table.name,
                                                    table.definition_page_number,
                                                    physical.root_page_number,
                                                    next_index_page_number,
                                                    {},
                                                    entries,
                                                    true,
                                                    primary || (physical.flags & index_unique_flag) != 0);
            next_index_page_number += applied.appended_pages.size();
            for (auto& page : applied.pages) {
                index_pages[page.page_number] = std::move(page.bytes);
            }
            appended_pages_by_index[physical_index].insert(
                appended_pages_by_index[physical_index].end(),
                applied.appended_pages.begin(),
                applied.appended_pages.end());
            appended_index_pages.insert(appended_index_pages.end(),
                                        applied.appended_pages.begin(),
                                        applied.appended_pages.end());
            index_entry_increments[physical_index] += applied.distinct_key_delta;
        }

        UsageBitmapAllocation usage_bitmaps{next_index_page_number, {}};
        // Each index owns the pages of its tree through its own usage-map row.
        for (std::size_t physical_index = 0;
             physical_index < appended_pages_by_index.size();
             ++physical_index) {
            if (appended_pages_by_index[physical_index].empty()) {
                continue;
            }
            const auto& physical = definition.physical_indexes[physical_index];
            const std::set<std::uint64_t> added(appended_pages_by_index[physical_index].begin(),
                                                appended_pages_by_index[physical_index].end());

            update_usage_map_row(file, table.name, data_pages, physical.owned_pages, added, {},
                                 false, &usage_bitmaps);
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

        update_usage_map_row(file, table.name, data_pages, definition.owned_pages, new_owned_pages, {},
                             true, &usage_bitmaps);
        update_usage_map_row(file, table.name, data_pages, definition.free_space_pages, add_free_pages,
                             removed_free_pages, false, &usage_bitmaps);
        update_long_value_usage_maps(file,
                                     table.name,
                                     definition,
                                     long_value_pages.pages_by_column,
                                     long_value_pages.full_pages_by_column,
                                     data_pages,
                                     usage_bitmaps);

        auto updated_definition_page = file.read_page(table.definition_page_number);
        write_u32(updated_definition_page,
                  16,
                  definition.row_count + static_cast<std::uint32_t>(rows.size()));
        for (std::size_t physical_index = 0;
             physical_index < definition.physical_indexes.size();
             ++physical_index) {
            const std::uint64_t count_offset = 63 + physical_index * 12 + 4;
            const std::int64_t next_count =
                static_cast<std::int64_t>(
                    definition.physical_indexes[physical_index].unique_entry_count) +
                index_entry_increments[physical_index];
            if (next_count < 0) {
                throw_table_error(DatabaseErrorCode::corrupted_file,
                                  file,
                                  table.name,
                                  "Index entry count would become negative",
                                  table.definition_page_number);
            }
            write_u32(updated_definition_page,
                      count_offset,
                      static_cast<std::uint32_t>(next_count));
        }
        std::vector<std::uint64_t> all_appended_pages = appended_page_numbers;
        all_appended_pages.insert(all_appended_pages.end(),
                                  appended_index_pages.begin(),
                                  appended_index_pages.end());
        all_appended_pages.insert(all_appended_pages.end(), usage_bitmaps.pages.begin(), usage_bitmaps.pages.end());
        const auto global_bitmap_pages = mark_global_pages_allocated(file, data_pages, all_appended_pages);

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
                                    for (const std::uint64_t page_number : appended_index_pages) {
                                        if (writer.append_page(index_pages.at(page_number)) != page_number) {
                                            throw_table_error(DatabaseErrorCode::io_error,
                                                              file,
                                                              table.name,
                                                              "Index page was appended at an unexpected page number",
                                                              page_number);
                                        }
                                    }
                                    append_usage_bitmap_pages(writer, data_pages, usage_bitmaps.pages);
                                    append_usage_bitmap_pages(writer, data_pages, global_bitmap_pages);
                                    for (const auto& entry : data_pages) {
                                        if (entry.first < file.page_count()) {
                                            writer.write_page(entry.first, entry.second);
                                        }
                                    }
                                    for (const auto& index_page : index_pages) {
                                        if (index_page.first < file.page_count()) {
                                            writer.write_page(index_page.first, index_page.second);
                                        }
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

}

void run_jet4_write_operation(Jet4WriteContext& context, const std::function<void()>& operation) {
    std::lock_guard<std::recursive_mutex> lock(context.write_mutex);
    PageWriteUndo undo(context.file.path(), context.file.page_count(), context.file.format().page_size);
    std::vector<std::uint32_t> row_counts;
    row_counts.reserve(context.schema.tables.size());
    for (const auto& table : context.schema.tables) {
        row_counts.push_back(table.definition.row_count);
    }

    context.undo = &undo;
    try {
        operation();
        context.undo = nullptr;
    } catch (...) {
        context.undo = nullptr;
        if (undo.dirty()) {
            try {
                undo.rollback();
                context.file = PageFile::open(context.file.path());
                for (std::size_t i = 0; i < row_counts.size(); ++i) {
                    context.schema.tables[i].definition.row_count = row_counts[i];
                }
            } catch (...) {
                context.write_state_valid = false;
                throw;
            }
        }
        throw;
    }
}

Jet4StagedPages stage_jet4_index(const PageFile& file, const ReadableTableSchema& table,
                                 const TableDefinition& definition, PhysicalIndexDefinition& physical,
                                 bool primary) {
    for (const auto& indexed : physical.columns) {
        if (indexed.column_number >= definition.columns.size() ||
            definition.columns[indexed.column_number].column_number != indexed.column_number ||
            !jet4_index_key_supported(definition.columns[indexed.column_number])) {
            unsupported_table_write(file, table.name, "Cannot encode a column in the new index");
        }
    }
    Jet4StagedPages staged;
    auto& pages = staged.pages;
    auto& appended = staged.appended;
    std::vector<Jet4IndexEntry> entries;
    TableRowReader reader(file, table.name, definition);
    while (const auto row = reader.next()) {
        auto key = encode_row_index_key(file, table, definition, physical, row->columns, &*row);
        if (includes_new_index_key(file, table.name, physical, primary, key)) {
            entries.push_back({std::move(key.bytes), static_cast<std::uint32_t>(row->page_number),
                               static_cast<std::uint8_t>(row->row_number), 0,
                               key.null_count == physical.columns.size()});
        }
    }
    const auto declaration_page_number = file.page_count();
    auto tree = build_jet4_index(file, table.definition_page_number, declaration_page_number + 1,
                                 entries, primary || (physical.flags & index_unique_flag) != 0);
    physical.root_page_number = static_cast<std::uint32_t>(tree.appended_pages.front());
    physical.unique_entry_count = static_cast<std::uint32_t>(tree.distinct_key_delta);
    physical.owned_pages = {0, static_cast<std::uint32_t>(declaration_page_number)};
    auto declaration = new_data_page(file.format().page_size, 0);
    append_row_to_page(declaration, std::vector<std::uint8_t>(69, 0));
    pages.emplace(declaration_page_number, std::move(declaration));
    for (auto& page : tree.pages) {
        pages.emplace(page.page_number, std::move(page.bytes));
    }
    appended.push_back(declaration_page_number);
    appended.insert(appended.end(), tree.appended_pages.begin(), tree.appended_pages.end());
    UsageBitmapAllocation usage_bitmaps{declaration_page_number + appended.size(), {}};
    const std::set<std::uint64_t> index_pages(tree.appended_pages.begin(), tree.appended_pages.end());
    update_usage_map_row(file, table.name, pages, physical.owned_pages,
                         index_pages, {}, false, &usage_bitmaps);
    appended.insert(appended.end(), usage_bitmaps.pages.begin(), usage_bitmaps.pages.end());
    const auto global_pages = mark_global_pages_allocated(file, pages, appended);
    appended.insert(appended.end(), global_pages.begin(), global_pages.end());
    return staged;
}

void write_jet4_schema_pages(
    Jet4WriteContext& context, const PageFile& file, const std::string& table_name,
    const Jet4StagedPages& staged,
    const std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>>& definitions) {
    write_pages_and_refresh(context, file, table_name, std::nullopt, std::nullopt,
                            [&](PageFileWriter& writer) {
        append_usage_bitmap_pages(writer, staged.pages, staged.appended);
        for (const auto& page : staged.pages) {
            if (page.first < file.page_count()) {
                writer.write_page(page.first, page.second);
            }
        }
        for (const auto& definition : definitions) {
            writer.write_page(definition.first, definition.second);
        }
    });
}

TableDefinition add_jet4_index(Jet4WriteContext& context, std::size_t table_index, const IndexBuilder& index) {
    const auto& table = context.schema.tables.at(table_index);
    if (!context.writable) {
        unsupported_table_write(context.file, table.name, "Index creation requires a writable database");
    }
    if (!context.write_state_valid) {
        throw_table_error(DatabaseErrorCode::io_error, context.file, table.name,
                          "Database write state is invalid after a previous write failure");
    }
    const auto file = PageFile::open(context.file.path());
    if (file.page_count() != context.file.page_count()) {
        throw_table_error(DatabaseErrorCode::io_error, file, table.name,
                          "Database file size changed outside this writable handle");
    }
    auto definition = read_current_table_definition(file, table);
    validate_local_table(file, table, definition);
    const auto same_name = [](const std::string& lhs, const std::string& rhs) {
        const auto lower_ascii = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
        return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                          [&](char a, char b) { return lower_ascii(a) == lower_ascii(b); });
    };
    if (index.name().empty() || index.name().size() > 64 ||
        !std::all_of(index.name().begin(), index.name().end(), [](unsigned char c) {
            return c >= 0x20 && c < 0x7f;
        })) {
        unsupported_table_write(file, table.name, "Index name must be printable ASCII and at most 64 characters");
    }
    if (index.columns().empty() || index.columns().size() > 10) {
        unsupported_table_write(file, table.name, "An index needs between one and ten columns");
    }

    LogicalIndexDefinition logical{};
    logical.name = index.name();
    logical.type = index.primary_key() ? primary_index_type : 0;
    logical.referenced_index_number = -1;
    logical.index_data_number = definition.physical_index_count;
    for (const auto& existing : definition.indexes) {
        if (same_name(existing.name, index.name()) ||
            (index.primary_key() && existing.type == primary_index_type)) {
            unsupported_table_write(file, table.name, "Index name or primary key already exists");
        }
        logical.index_number = std::max(logical.index_number, existing.index_number + 1);
    }

    PhysicalIndexDefinition physical{};
    physical.flags = static_cast<std::uint8_t>((index.unique() ? index_unique_flag : 0) |
                                              (index.required() ? index_required_flag : 0) |
                                              (index.ignore_nulls() ? index_ignore_nulls_flag : 0));
    std::set<std::uint16_t> seen_columns;
    for (const auto& indexed : index.columns()) {
        const auto column = std::find_if(definition.columns.begin(), definition.columns.end(),
                                         [&](const auto& candidate) {
            return same_name(candidate.name, indexed.name);
        });
        if (column == definition.columns.end() || !jet4_index_key_supported(*column) ||
            !seen_columns.insert(column->column_number).second) {
            unsupported_table_write(file, table.name, "Unknown, duplicate or unsupported index column");
        }
        if (column->column_number != static_cast<std::size_t>(column - definition.columns.begin())) {
            unsupported_table_write(file, table.name, "Index creation requires contiguous column numbers");
        }
        physical.columns.push_back({column->column_number, indexed.ascending});
    }

    const PhysicalIndexDefinition* added_physical = &physical;
    for (std::size_t number = 0; number < definition.physical_indexes.size(); ++number) {
        const auto& existing = definition.physical_indexes[number];
        if (existing.flags == physical.flags && existing.extended_flags == 0 &&
            std::equal(existing.columns.begin(), existing.columns.end(),
                       physical.columns.begin(), physical.columns.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.column_number == rhs.column_number && lhs.ascending == rhs.ascending;
            })) {
            logical.index_data_number = static_cast<std::uint32_t>(number);
            added_physical = nullptr;
            break;
        }
    }
    auto staged = added_physical
        ? stage_jet4_index(file, table, definition, physical, index.primary_key())
        : Jet4StagedPages{};
    auto definition_write = definition_with_added_jet4_index(
        file, table, definition, logical, added_physical,
        file.page_count() + staged.appended.size());
    stage_definition_write(file, staged, std::move(definition_write));
    write_jet4_schema_pages(context, file, table.name, staged, {});
    auto updated = read_current_table_definition(context.file, table);
    updated.column_properties_available = table.definition.column_properties_available;
    return updated;
}

std::vector<RowId> append_jet4_rows(Jet4WriteContext& context,
                                    std::size_t table_index,
                                    const std::vector<std::vector<Value>>& rows) {
    return append_rows_impl(context, table_index, rows, false);
}

std::vector<RowId> append_jet4_catalog_rows(Jet4WriteContext& context,
                                            std::size_t table_index,
                                            const std::vector<std::vector<Value>>& rows) {
    return append_rows_impl(context, table_index, rows, true);
}

void update_jet4_row_impl(Jet4WriteContext& context,
                          std::size_t table_index,
                          RowId row_id,
                          const std::vector<Value>& values,
                          CascadeSeed& seed,
                          const std::vector<std::uint8_t>* property_bytes = nullptr) {
    if (!context.writable) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Database was opened read-only; reopen it in read-write mode to update rows",
                            context.file.path());
    }
    std::lock_guard<std::recursive_mutex> write_lock(context.write_mutex);
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
        validate_local_table(file, table, definition, property_bytes != nullptr);
        const std::uint64_t fixed_data_size =
            fixed_row_data_size(file, table, definition, property_bytes != nullptr);

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
        const EncodedTableRow old_row = row_reader.read_at(row_id.page_number, row_id.row_number);
        std::optional<std::vector<Value>> validated_values;
        if (!property_bytes && has_column_validators(context, table_index)) {
            validated_values = values;
            apply_column_validators(context, table_index, *validated_values);
        }
        const auto& effective_values = validated_values ? *validated_values : values;
        std::optional<std::uint16_t> legacy_property_column;
        if (property_bytes) {
            const auto column = std::find_if(definition.columns.begin(), definition.columns.end(),
                                            [](const auto& item) {
                return item.name == "LvProp" && item.type == 0x0b && (item.flags & 1) == 0;
            });
            if (column == definition.columns.end() || property_bytes->size() > long_value_length_mask) {
                unsupported_table_write(file, table.name, "Property update exceeds supported LvProp storage");
            }
            if (!column->long_value_owned_pages && !column->long_value_free_space_pages) {
                legacy_property_column = column->column_number;
            }
            for (const auto& index : definition.physical_indexes) {
                if (std::any_of(index.columns.begin(), index.columns.end(), [&](const auto& indexed) {
                        return indexed.column_number == column->column_number;
                    })) {
                    unsupported_table_write(file, table.name, "Cannot update indexed catalog properties");
                }
            }
        } else {
            seed.capture(decode_row_values(file, table, definition, old_row));
            validate_foreign_keys(file, context.schema, table, definition, &old_row, &effective_values);
        }
        auto encoded_row = encode_row(file, table, definition, effective_values, fixed_data_size,
                                      property_bytes ? &old_row : nullptr, property_bytes);
        if (!property_bytes) {
            validate_column_rules(file, table, definition, effective_values);
            validate_table_rule(file, table, definition, effective_values);
        }
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
                                                                      definition,
                                                                      encoded_row,
                                                                      file.page_count());
        if (legacy_property_column) {
            // Jet4 catalogs may use the legacy LVAL layout without column usage maps.
            // The allocated pages remain tracked by the global usage map.
            long_value_pages.pages_by_column.erase(*legacy_property_column);
        }
        Jet4IndexPageUpdates index_updates;
        if (!property_bytes) {
            maintain_index_key_change(file,
                                      table,
                                      definition,
                                      index_updates,
                                      old_row,
                                      row_id,
                                      &effective_values);
        }
        const auto merge_index_updates =
            [&](std::map<std::uint64_t, std::vector<std::uint8_t>>& pages) {
                for (auto& entry : index_updates.pages) {
                    pages[entry.first] = std::move(entry.second);
                }
                apply_index_count_deltas(file,
                                         table.name,
                                         table.definition_page_number,
                                         definition,
                                         index_updates,
                                         pages);
            };
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
            UsageBitmapAllocation usage_bitmaps{long_value_pages.next_page_number, {}};
            merge_index_updates(updated_pages);
            update_long_value_usage_maps(file,
                                         table.name,
                                         definition,
                                         long_value_pages.pages_by_column,
                                         long_value_pages.full_pages_by_column,
                                         updated_pages,
                                         usage_bitmaps);
            std::vector<std::uint64_t> appended_page_numbers(long_value_pages.appended_pages.begin(),
                                                            long_value_pages.appended_pages.end());
            appended_page_numbers.insert(appended_page_numbers.end(), usage_bitmaps.pages.begin(), usage_bitmaps.pages.end());
            const auto global_bitmap_pages = mark_global_pages_allocated(file, updated_pages, appended_page_numbers);
            release_unreferenced_long_value_pages(file,
                                                  table.name,
                                                  definition,
                                                  row_id,
                                                  old_row,
                                                  &encoded_row.bytes,
                                                  long_value_pages.pages,
                                                  updated_pages);

            write_pages_and_refresh(context,
                                    file,
                                    table.name,
                                    row_id.row_number,
                                    row_id.page_number,
                                    [&](PageFileWriter& writer) {
                                        for (const auto& page : long_value_pages.pages) {
                                            if (long_value_pages.appended_pages.count(page.first) == 0) {
                                                writer.write_page(page.first, page.second);
                                                continue;
                                            }
                                            if (writer.append_page(page.second) != page.first) {
                                                throw_table_error(DatabaseErrorCode::io_error,
                                                                  file,
                                                                  table.name,
                                                                  "Long-value page was appended at an unexpected page number",
                                                                  page.first);
                                            }
                                        }
                                        append_usage_bitmap_pages(writer, updated_pages, usage_bitmaps.pages);
                                        append_usage_bitmap_pages(writer, updated_pages, global_bitmap_pages);
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

            UsageBitmapAllocation usage_bitmaps{long_value_pages.next_page_number, {}};
            merge_index_updates(updated_pages);
            const auto free_page_list = read_usage_map_pages(file, definition.free_space_pages);
            const std::set<std::uint64_t> free_pages(free_page_list.begin(), free_page_list.end());
            if (free_pages.find(final_row.page_number) == free_pages.end() &&
                row_fits_on_data_page(file,
                                      table.name,
                                      updated_pages.at(final_row.page_number),
                                      encoded_row.bytes.size())) {
                const std::set<std::uint64_t> newly_free_pages{final_row.page_number};
                update_usage_map_row(file,
                                     table.name,
                                     updated_pages,
                                     definition.free_space_pages,
                                     newly_free_pages,
                                     {},
                                     true,
                                     &usage_bitmaps);
            }
            update_long_value_usage_maps(file,
                                         table.name,
                                         definition,
                                         long_value_pages.pages_by_column,
                                         long_value_pages.full_pages_by_column,
                                         updated_pages,
                                         usage_bitmaps);
            std::vector<std::uint64_t> appended_page_numbers(long_value_pages.appended_pages.begin(),
                                                            long_value_pages.appended_pages.end());
            appended_page_numbers.insert(appended_page_numbers.end(), usage_bitmaps.pages.begin(), usage_bitmaps.pages.end());
            const auto global_bitmap_pages = mark_global_pages_allocated(file, updated_pages, appended_page_numbers);
            release_unreferenced_long_value_pages(file,
                                                  table.name,
                                                  definition,
                                                  row_id,
                                                  old_row,
                                                  &encoded_row.bytes,
                                                  long_value_pages.pages,
                                                  updated_pages);

            write_pages_and_refresh(context,
                                    file,
                                    table.name,
                                    row_id.row_number,
                                    row_id.page_number,
                                    [&](PageFileWriter& writer) {
                                        for (const auto& page : long_value_pages.pages) {
                                            if (long_value_pages.appended_pages.count(page.first) == 0) {
                                                writer.write_page(page.first, page.second);
                                                continue;
                                            }
                                            if (writer.append_page(page.second) != page.first) {
                                                throw_table_error(DatabaseErrorCode::io_error,
                                                                  file,
                                                                  table.name,
                                                                  "Long-value page was appended at an unexpected page number",
                                                                  page.first);
                                            }
                                        }
                                        append_usage_bitmap_pages(writer, updated_pages, usage_bitmaps.pages);
                                        append_usage_bitmap_pages(writer, updated_pages, global_bitmap_pages);
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

        UsageBitmapAllocation usage_bitmaps{long_value_pages.next_page_number + appended_data_page_numbers.size(), {}};
        merge_index_updates(updated_pages);
        update_usage_map_row(file, table.name, updated_pages, definition.owned_pages, new_owned_pages, {},
                             true, &usage_bitmaps);
        update_usage_map_row(file, table.name, updated_pages, definition.free_space_pages, add_free_pages,
                             remove_free_pages, false, &usage_bitmaps);

        update_long_value_usage_maps(file,
                                     table.name,
                                     definition,
                                     long_value_pages.pages_by_column,
                                     long_value_pages.full_pages_by_column,
                                     updated_pages,
                                     usage_bitmaps);
        std::vector<std::uint64_t> appended_page_numbers(long_value_pages.appended_pages.begin(),
                                                         long_value_pages.appended_pages.end());
        appended_page_numbers.insert(appended_page_numbers.end(),
                                     appended_data_page_numbers.begin(),
                                     appended_data_page_numbers.end());
        appended_page_numbers.insert(appended_page_numbers.end(), usage_bitmaps.pages.begin(), usage_bitmaps.pages.end());
        const auto global_bitmap_pages = mark_global_pages_allocated(file, updated_pages, appended_page_numbers);
        release_unreferenced_long_value_pages(file,
                                              table.name,
                                              definition,
                                              row_id,
                                              old_row,
                                              &encoded_row.bytes,
                                              long_value_pages.pages,
                                              updated_pages);

        write_pages_and_refresh(context,
                                file,
                                table.name,
                                row_id.row_number,
                                row_id.page_number,
                                [&](PageFileWriter& writer) {
                                    for (const auto& page : long_value_pages.pages) {
                                        if (long_value_pages.appended_pages.count(page.first) == 0) {
                                            writer.write_page(page.first, page.second);
                                            continue;
                                        }
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
                                    append_usage_bitmap_pages(writer, updated_pages, usage_bitmaps.pages);
                                    append_usage_bitmap_pages(writer, updated_pages, global_bitmap_pages);
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

void delete_jet4_row_impl(Jet4WriteContext& context,
                          std::size_t table_index,
                          RowId row_id,
                          CascadeSeed& seed) {
    if (!context.writable) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Database was opened read-only; reopen it in read-write mode to delete rows",
                            context.file.path());
    }
    std::lock_guard<std::recursive_mutex> write_lock(context.write_mutex);
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
        validate_local_table(file, table, definition);
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
        const EncodedTableRow deleted_row = row_reader.read_at(row_id.page_number, row_id.row_number);
        seed.capture(decode_row_values(file, table, definition, deleted_row));
        validate_foreign_keys(file, context.schema, table, definition, &deleted_row, nullptr);

        const std::uint64_t row_location_offset =
            data_page_row_directory_offset + static_cast<std::uint64_t>(row_id.row_number) * 2;
        const std::uint16_t row_location = read_u16(page, row_location_offset);
        write_u16(page,
                  row_location_offset,
                  static_cast<std::uint16_t>(row_location | data_page_deleted_row_mask |
                                             data_page_overflow_row_mask));

        Jet4IndexPageUpdates index_updates;
        maintain_index_key_change(file,
                                  table,
                                  definition,
                                  index_updates,
                                  deleted_row,
                                  row_id,
                                  nullptr);

        std::map<std::uint64_t, std::vector<std::uint8_t>> updated_pages;
        updated_pages.emplace(row_id.page_number, std::move(page));
        for (auto& entry : index_updates.pages) {
            updated_pages[entry.first] = std::move(entry.second);
        }
        auto& updated_definition_page = mutable_page(updated_pages, file, table.definition_page_number);
        write_u32(updated_definition_page, 16, definition.row_count - 1);
        for (const auto& entry : index_updates.count_deltas) {
            if (entry.second == 0 || entry.first >= definition.physical_indexes.size()) {
                continue;
            }
            const std::int64_t next =
                static_cast<std::int64_t>(definition.physical_indexes[entry.first].unique_entry_count) +
                entry.second;
            if (next < 0) {
                throw_table_error(DatabaseErrorCode::corrupted_file,
                                  file,
                                  table.name,
                                  "Index entry count would become negative",
                                  table.definition_page_number);
            }
            write_u32(updated_definition_page,
                      63 + entry.first * 12 + 4,
                      static_cast<std::uint32_t>(next));
        }
        release_unreferenced_long_value_pages(file,
                                              table.name,
                                              definition,
                                              row_id,
                                              deleted_row,
                                              nullptr,
                                              {},
                                              updated_pages);
        write_pages_and_refresh(context,
                                file,
                                table.name,
                                row_id.row_number,
                                row_id.page_number,
                                [&](PageFileWriter& writer) {
                                    for (const auto& entry : updated_pages) {
                                        writer.write_page(entry.first, entry.second);
                                    }
                                });
        context.schema.tables[table_index].definition.row_count = definition.row_count - 1;
    } catch (const DatabaseError& error) {
        rethrow_with_table_context(context.file, table.name, error, row_id.row_number);
    }
}


// Applies the cascade actions that the parent side of each local relationship declares. It runs
// after the parent row has been written or deleted, so a failure part way through leaves the
// file without those later cascades; this writer still has no transaction support.
void cascade_parent_dependents(Jet4WriteContext& context,
                               std::size_t table_index,
                               const std::vector<Value>& old_values,
                               const std::vector<Value>* new_values) {
    constexpr std::uint8_t foreign_index_type = 2;
    constexpr std::uint8_t foreign_parent_type = 1;
    constexpr std::uint8_t cascade_flag = 0x01;
    constexpr std::uint8_t cascade_null_flag = 0x02;
    constexpr int maximum_cascade_depth = 16;
    static thread_local int cascade_depth = 0;
    if (cascade_depth >= maximum_cascade_depth) {
        throw DatabaseError(DatabaseErrorCode::unsupported_operation,
                            "Foreign key cascade exceeded the supported depth",
                            context.file.path());
    }
    struct DepthGuard {
        int& depth;
        explicit DepthGuard(int& value) : depth(value) { ++depth; }
        ~DepthGuard() { --depth; }
    } guard(cascade_depth);

    const auto& table = context.schema.tables.at(table_index);
    const PageFile file = PageFile::open(context.file.path());
    const TableDefinition definition = read_current_table_definition(file, table);
    for (const auto& link : definition.indexes) {
        if (link.type != foreign_index_type || link.referenced_index_type != foreign_parent_type ||
            link.referenced_index_number < 0) {
            continue;
        }
        const bool updating = new_values != nullptr;
        const bool cascades = updating ? (link.cascade_updates_flags & cascade_flag) != 0
                                       : (link.cascade_deletes_flags & cascade_flag) != 0;
        const bool nulls_on_delete = !updating && (link.cascade_deletes_flags & cascade_null_flag) != 0;
        if (!cascades && !nulls_on_delete) {
            continue;
        }
        const auto child = std::find_if(context.schema.tables.begin(), context.schema.tables.end(),
                                        [&](const auto& other) {
            return other.definition_page_number == link.referenced_table_page_number && !other.is_linked;
        });
        if (child == context.schema.tables.end()) {
            unsupported_table_write(file, table.name, "Foreign key references an unavailable local table");
        }
        const auto child_position = static_cast<std::size_t>(child - context.schema.tables.begin());
        const TableDefinition child_definition = read_current_table_definition(file, *child);
        const auto reciprocal = std::find_if(child_definition.indexes.begin(),
                                            child_definition.indexes.end(), [&](const auto& other) {
            return other.index_number == static_cast<std::uint32_t>(link.referenced_index_number);
        });
        if (reciprocal == child_definition.indexes.end() || reciprocal->type != foreign_index_type) {
            throw_table_error(DatabaseErrorCode::corrupted_file, file, table.name,
                              "Foreign key reciprocal index is missing or inconsistent");
        }
        const auto& local_index =
            foreign_physical_index(file, table.name, definition, link.index_data_number);
        const auto& child_index =
            foreign_physical_index(file, table.name, child_definition, reciprocal->index_data_number);
        if (local_index.columns.empty() || local_index.columns.size() != child_index.columns.size()) {
            throw_table_error(DatabaseErrorCode::corrupted_file, file, table.name,
                              "Foreign key index column counts are inconsistent");
        }
        EncodedIndexKey key;
        for (std::size_t i = 0; i < local_index.columns.size(); ++i) {
            const auto& local_column = foreign_index_column(file, table.name, definition,
                                                           local_index.columns[i].column_number);
            const auto& indexed = child_index.columns[i];
            const auto child_column = std::find_if(child_definition.columns.begin(),
                                                   child_definition.columns.end(), [&](const auto& column) {
                return column.column_number == indexed.column_number;
            });
            if (child_column == child_definition.columns.end() || local_column.type != child_column->type) {
                throw_table_error(DatabaseErrorCode::corrupted_file, file, table.name,
                                  "Foreign key index column types differ");
            }
            const Value& value = old_values.at(local_column.column_number);
            if (std::holds_alternative<std::monostate>(value)) {
                ++key.null_count;
            }
            try {
                const auto part = encode_jet4_index_key(*child_column, value, !indexed.ascending);
                key.bytes.insert(key.bytes.end(), part.begin(), part.end());
            } catch (const std::invalid_argument& error) {
                unsupported_table_write(file, table.name, error.what());
            }
        }
        key.bytes = truncate_jet4_index_key(std::move(key.bytes));
        if (key.null_count == local_index.columns.size()) {
            continue;
        }
        std::vector<RowId> dependent_rows;
        IndexEntryCursor cursor(file,
                                child->name,
                                child->definition_page_number,
                                child_index.root_page_number,
                                key.bytes);
        while (const auto entry = cursor.next_entry()) {
            if (entry->key != key.bytes) {
                break;
            }
            dependent_rows.push_back({entry->row.page_number, entry->row.row_number});
        }
        for (const auto& dependent : dependent_rows) {
            if (!updating && cascades) {
                delete_jet4_row(context, child_position, dependent);
                continue;
            }
            const PageFile current = PageFile::open(context.file.path());
            const TableDefinition current_child = read_current_table_definition(current, *child);
            TableRowReader reader(current, child->name, current_child);
            const EncodedTableRow row = reader.read_at(dependent.page_number, dependent.row_number);
            auto values = decode_row_values(current, *child, current_child, row);
            for (std::size_t i = 0; i < child_index.columns.size(); ++i) {
                const auto column_number = child_index.columns[i].column_number;
                if (column_number >= values.size()) {
                    throw_table_error(DatabaseErrorCode::corrupted_file, current, child->name,
                                      "Foreign key index references a column outside the table definition");
                }
                values[column_number] = nulls_on_delete
                                            ? Value{}
                                            : new_values->at(local_index.columns[i].column_number);
            }
            update_jet4_row(context, child_position, dependent, values);
        }
    }
}

void update_jet4_catalog_properties(Jet4WriteContext& context, std::size_t table_index,
                                    RowId row_id, const std::vector<std::uint8_t>& bytes) {
    const auto& table = context.schema.tables.at(table_index);
    if (table.is_user_table || table.definition_page_number != 2) {
        throw std::invalid_argument("Expected the MSysObjects catalog");
    }
    CascadeSeed seed;
    update_jet4_row_impl(context, table_index, row_id, {}, seed, &bytes);
}

void update_jet4_row(Jet4WriteContext& context,
                     std::size_t table_index,
                     RowId row_id,
                     const std::vector<Value>& values) {
    CascadeSeed seed;
    update_jet4_row_impl(context, table_index, row_id, values, seed);
    if (!seed.captured) {
        return;
    }
    cascade_parent_dependents(context, table_index, seed.values, &values);
}

void delete_jet4_row(Jet4WriteContext& context, std::size_t table_index, RowId row_id) {
    CascadeSeed seed;
    delete_jet4_row_impl(context, table_index, row_id, seed);
    if (!seed.captured) {
        return;
    }
    cascade_parent_dependents(context, table_index, seed.values, nullptr);
}

}
