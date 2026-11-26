#include "canonical_json.hpp"

#include "catalog/table_catalog.hpp"
#include "format/jet_format.hpp"
#include "value/jet4_value.hpp"
#include "util/sha256.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace rea_jackcess::detail {
namespace {

struct CanonicalError {
    std::optional<std::string> stage;
    std::optional<std::string> table_name;
    std::optional<std::string> column_name;
    std::string message;
    std::optional<std::uint64_t> page_number;
    std::optional<std::uint64_t> row_number;
};

void write_json_string(std::ostream& output, std::string_view value) {
    constexpr char hex_digits[] = "0123456789abcdef";
    output << '"';
    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20) {
                    output << "\\u00" << hex_digits[character >> 4] << hex_digits[character & 0x0f];
                } else {
                    output << static_cast<char>(character);
                }
                break;
        }
    }
    output << '"';
}

void write_optional_string(std::ostream& output, const std::optional<std::string>& value) {
    if (value) {
        write_json_string(output, *value);
    } else {
        output << "null";
    }
}

template <typename T>
void write_optional_number(std::ostream& output, const std::optional<T>& value) {
    if (value) {
        output << *value;
    } else {
        output << "null";
    }
}

std::optional<std::string_view> canonical_type(std::uint8_t mdb_type) {
    switch (mdb_type) {
        case 0x01:
            return "bool";
        case 0x02:
            return "uint8";
        case 0x03:
            return "int16";
        case 0x04:
            return "int32";
        case 0x05:
            return "money";
        case 0x06:
            return "float32";
        case 0x07:
            return "float64";
        case 0x08:
        case 0x14:
            return "datetime";
        case 0x09:
        case 0x11:
            return "binary";
        case 0x0a:
            return "text";
        case 0x0b:
            return "ole";
        case 0x0c:
            return "memo";
        case 0x0f:
            return "guid";
        case 0x10:
            return "numeric";
        case 0x12:
            // A complex column stores a 4-byte foreign key into its flat table.
            return "complex";
        case 0x13:
            return "int64";
        default:
            return std::nullopt;
    }
}

std::string_view mdb_type_name(std::uint8_t mdb_type) {
    switch (mdb_type) {
        case 0x01:
            return "BOOLEAN";
        case 0x02:
            return "BYTE";
        case 0x03:
            return "INT";
        case 0x04:
            return "LONG";
        case 0x05:
            return "MONEY";
        case 0x06:
            return "FLOAT";
        case 0x07:
            return "DOUBLE";
        case 0x08:
            return "SHORT_DATE_TIME";
        case 0x09:
            return "BINARY";
        case 0x0a:
            return "TEXT";
        case 0x0b:
            return "OLE";
        case 0x0c:
            return "MEMO";
        case 0x0f:
            return "GUID";
        case 0x10:
            return "NUMERIC";
        case 0x11:
            return "BIG_BINARY";
        case 0x12:
            return "COMPLEX_TYPE";
        case 0x13:
            return "BIG_INT";
        case 0x14:
            return "EXT_DATE_TIME";
        default:
            return "UNKNOWN";
    }
}

std::pair<unsigned int, unsigned int> canonical_precision_scale(const ColumnDefinition& column) {
    switch (column.type) {
        case 0x05:
            return {19, 4};
        case 0x10:
            return {column.precision, column.scale};
        default:
            return {0, 0};
    }
}

template <typename Float>
void write_floating_point(std::ostream& output, Float value) {
    if (std::isnan(value)) {
        output << "{\"$float\":\"NaN\"}";
        return;
    }
    if (std::isinf(value)) {
        output << (value > 0 ? "{\"$float\":\"Infinity\"}" : "{\"$float\":\"-Infinity\"}");
        return;
    }
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                      std::chars_format::general,
                                      std::numeric_limits<Float>::max_digits10);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("Could not format a floating-point value as JSON");
    }
    output.write(buffer.data(), result.ptr - buffer.data());
}

void write_canonical_value(std::ostream& output, const Jet4Value& value) {
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            output << "null";
        } else if constexpr (std::is_same_v<T, bool>) {
            output << (item ? "true" : "false");
        } else if constexpr (std::is_same_v<T, std::int8_t>) {
            output << static_cast<int>(item);
        } else if constexpr (std::is_same_v<T, std::int16_t> ||
                             std::is_same_v<T, std::int32_t> ||
                             std::is_same_v<T, std::int64_t>) {
            output << item;
        } else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
            write_floating_point(output, item);
        } else if constexpr (std::is_same_v<T, std::string>) {
            write_json_string(output, item);
        } else if constexpr (std::is_same_v<T, DecimalValue>) {
            output << "{\"$decimal\":";
            write_json_string(output, item.text);
            output << '}';
        } else if constexpr (std::is_same_v<T, DateTimeValue>) {
            output << "{\"$date\":";
            write_json_string(output, item.iso8601);
            output << '}';
        } else if constexpr (std::is_same_v<T, DateOutOfRangeValue>) {
            output << "{\"$dateOutOfRange\":";
            write_floating_point(output, item.serial);
            output << '}';
        } else if constexpr (std::is_same_v<T, GuidValue>) {
            output << "{\"$guid\":";
            write_json_string(output, item.text);
            output << '}';
        } else if constexpr (std::is_same_v<T, MemoValue>) {
            write_json_string(output, item.text);
        } else if constexpr (std::is_same_v<T, BlobValue>) {
            output << "{\"$blob\":{\"size\":" << item.bytes.size() << ",\"sha256\":";
            write_json_string(output, sha256_hex(item.bytes));
            output << "}}";
        } else if constexpr (std::is_same_v<T, UnsupportedValue>) {
            output << "{\"$unsupported\":";
            write_json_string(output, item.description);
            output << '}';
        }
    }, value);
}

struct SerializedTableRows {
    std::vector<std::string> rows;
    std::string row_set_hash;
};

SerializedTableRows serialize_table_rows(const PageFile& file, const ReadableTableSchema& table) {
    const auto encoded_rows = read_table_rows(file, table.name, table.definition);
    SerializedTableRows result;
    result.rows.reserve(encoded_rows.size());
    for (const auto& row : encoded_rows) {
        std::ostringstream serialized_row;
        serialized_row << '[';
        for (std::size_t index = 0; index < table.definition.columns.size(); ++index) {
            if (index != 0) {
                serialized_row << ',';
            }
            const auto& column = table.definition.columns[index];
            write_canonical_value(serialized_row, decode_jet4_value(file, table.name, row, column));
        }
        serialized_row << ']';
        result.rows.push_back(serialized_row.str());
    }

    std::vector<std::string> row_hashes;
    row_hashes.reserve(result.rows.size());
    for (const auto& row : result.rows) {
        const std::vector<std::uint8_t> bytes(row.begin(), row.end());
        row_hashes.push_back(sha256_hex(bytes));
    }
    std::sort(row_hashes.begin(), row_hashes.end());
    std::string combined_hash_input;
    combined_hash_input.reserve(row_hashes.size() * 64);
    for (const auto& hash : row_hashes) {
        combined_hash_input += hash;
    }
    const std::vector<std::uint8_t> combined_bytes(combined_hash_input.begin(), combined_hash_input.end());
    result.row_set_hash = sha256_hex(combined_bytes);
    return result;
}

void write_schema_table(std::ostream& output,
                        const ReadableTableSchema& table,
                        const SerializedTableRows* rows) {
    output << "{\"name\":";
    write_json_string(output, table.name);
    output << ",\"objectType\":";
    write_json_string(output, table.object_type);
    output << ",\"isUserTable\":" << (table.is_user_table ? "true" : "false")
           << ",\"isLinked\":" << (table.is_linked ? "true" : "false")
           << ",\"rowCountHeader\":" << table.definition.row_count << ",\"columns\":[";
    for (std::size_t index = 0; index < table.definition.columns.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        const auto& column = table.definition.columns[index];
        const auto [precision, scale] = canonical_precision_scale(column);
        output << "{\"ordinal\":" << index + 1 << ",\"name\":";
        write_json_string(output, column.name);
        output << ",\"mdbType\":";
        write_json_string(output, mdb_type_name(column.type));
        output << ",\"canonicalType\":";
        const auto normalized_type = canonical_type(column.type);
        if (normalized_type) {
            write_json_string(output, *normalized_type);
        } else {
            write_json_string(output, "unknown");
        }
        output << ",\"size\":" << column.length
               << ",\"precision\":" << precision
               << ",\"scale\":" << scale
               << ",\"fixed\":" << ((column.flags & 0x01) != 0 ? "true" : "false")
               << ",\"rowColNum\":" << index
               << ",\"nullable\":";
        if (column.nullable) {
            output << (*column.nullable ? "true" : "false");
        } else {
            output << "null";
        }
        output << '}';
    }
    output << "],\"indexes\":[";
    for (std::size_t index = 0; index < table.definition.indexes.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        const auto& logical = table.definition.indexes[index];
        const auto& physical = table.definition.physical_indexes[logical.index_data_number];
        const bool primary_key = logical.type == 0x01;
        const bool foreign_key = logical.type == 0x02;
        const bool physical_is_primary =
            std::any_of(table.definition.indexes.begin(), table.definition.indexes.end(), [&](const auto& item) {
                return item.index_data_number == logical.index_data_number && item.type == 0x01;
            });
        output << "{\"ordinal\":" << index + 1 << ",\"name\":";
        write_json_string(output, logical.name);
        output << ",\"indexNumber\":" << logical.index_number
               << ",\"physicalIndexNumber\":" << logical.index_data_number
               << ",\"indexType\":" << static_cast<unsigned int>(logical.type)
               << ",\"primaryKey\":" << (primary_key ? "true" : "false")
               << ",\"foreignKey\":" << (foreign_key ? "true" : "false")
               << ",\"unique\":" << (physical_is_primary || (physical.flags & 0x01) != 0 ? "true" : "false")
               << ",\"ignoreNulls\":" << ((physical.flags & 0x02) != 0 ? "true" : "false")
               << ",\"required\":" << ((physical.flags & 0x08) != 0 ? "true" : "false")
               << ",\"flags\":" << static_cast<unsigned int>(physical.flags)
               << ",\"rootPage\":" << physical.root_page_number
               << ",\"uniqueEntryCount\":" << physical.unique_entry_count
               << ",\"columns\":[";
        for (std::size_t column_index = 0; column_index < physical.columns.size(); ++column_index) {
            if (column_index != 0) {
                output << ',';
            }
            const auto& indexed_column = physical.columns[column_index];
            const auto column = std::find_if(table.definition.columns.begin(),
                                             table.definition.columns.end(),
                                             [&](const auto& candidate) {
                                                 return candidate.column_number == indexed_column.column_number;
                                             });
            output << "{\"ordinal\":" << column_index + 1
                   << ",\"columnNumber\":" << indexed_column.column_number << ",\"name\":";
            write_json_string(output, column->name);
            output << ",\"ascending\":" << (indexed_column.ascending ? "true" : "false") << '}';
        }
        output << "]";
        if (foreign_key && logical.referenced_index_number >= 0) {
            output << ",\"reference\":{\"indexType\":"
                   << static_cast<unsigned int>(logical.referenced_index_type)
                   << ",\"indexNumber\":" << logical.referenced_index_number
                   << ",\"tablePage\":" << logical.referenced_table_page_number
                   << ",\"cascadeUpdates\":" << ((logical.cascade_updates_flags & 0x01) != 0 ? "true" : "false")
                   << ",\"cascadeDeletes\":" << ((logical.cascade_deletes_flags & 0x01) != 0 ? "true" : "false")
                   << ",\"cascadeSetNull\":" << ((logical.cascade_deletes_flags & 0x02) != 0 ? "true" : "false")
                   << '}';
        } else {
            output << ",\"reference\":null";
        }
        output << '}';
    }
    output << "],\"physicalIndexes\":[";
    for (std::size_t index = 0; index < table.definition.physical_indexes.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        const auto& physical = table.definition.physical_indexes[index];
        output << "{\"ordinal\":" << index + 1
               << ",\"rootPage\":" << physical.root_page_number
               << ",\"uniqueEntryCount\":" << physical.unique_entry_count
               << ",\"flags\":" << static_cast<unsigned int>(physical.flags)
               << ",\"extendedFlags\":" << static_cast<unsigned int>(physical.extended_flags)
               << ",\"ownedPages\":{\"row\":" << static_cast<unsigned int>(physical.owned_pages.row_number)
               << ",\"page\":" << physical.owned_pages.page_number << "},\"columns\":[";
        for (std::size_t column_index = 0; column_index < physical.columns.size(); ++column_index) {
            if (column_index != 0) {
                output << ',';
            }
            const auto& indexed_column = physical.columns[column_index];
            const auto column = std::find_if(table.definition.columns.begin(),
                                             table.definition.columns.end(),
                                             [&](const auto& candidate) {
                                                 return candidate.column_number == indexed_column.column_number;
                                             });
            output << "{\"columnNumber\":" << indexed_column.column_number << ",\"name\":";
            write_json_string(output, column->name);
            output << ",\"ascending\":" << (indexed_column.ascending ? "true" : "false") << '}';
        }
        output << "]}";
    }
    output << ']';
    if (rows == nullptr) {
        output << ",\"rows\":null}";
        return;
    }
    output << ",\"rowCount\":" << rows->rows.size()
           << ",\"truncated\":false,\"rowSetHash\":";
    write_json_string(output, rows->row_set_hash);
    output << ",\"rows\":[";
    for (std::size_t index = 0; index < rows->rows.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << rows->rows[index];
    }
    output << "]}";
}

void write_error(std::ostream& output, const CanonicalError& error) {
    output << '{';
    bool has_field = false;
    const auto write_separator = [&] {
        if (has_field) {
            output << ',';
        }
        has_field = true;
    };
    if (error.stage) {
        write_separator();
        output << "\"stage\":";
        write_json_string(output, *error.stage);
    }
    if (error.table_name) {
        write_separator();
        output << "\"table\":";
        write_json_string(output, *error.table_name);
    }
    if (error.column_name) {
        write_separator();
        output << "\"column\":";
        write_json_string(output, *error.column_name);
    }
    write_separator();
    output << "\"message\":";
    write_json_string(output, error.message);
    if (error.page_number) {
        write_separator();
        output << "\"page\":" << *error.page_number;
    }
    if (error.row_number) {
        write_separator();
        output << "\"row\":" << *error.row_number;
    }
    output << '}';
}

}

void write_canonical_json(std::ostream& output,
                          const PageFile& file,
                          const std::filesystem::path& source_path) {
    std::vector<CanonicalError> errors;
    const auto alias = source_path.filename().u8string();
    std::error_code size_error;
    const auto byte_count = std::filesystem::file_size(source_path, size_error);
    const std::optional<std::uint64_t> stored_byte_count =
        size_error ? std::nullopt : std::optional<std::uint64_t>(byte_count);
    if (size_error) {
        errors.push_back({"file", std::nullopt, std::nullopt,
                          "Cannot determine source file size: " + size_error.message(), std::nullopt});
    }

    std::optional<std::string> file_hash;
    try {
        file_hash = sha256_file_hex(source_path);
    } catch (const DatabaseError& error) {
        errors.push_back({"file", std::nullopt, std::nullopt, error.what(), error.page_number()});
    }

    std::optional<std::uint16_t> code_page;
    if (file.format().version == EngineVersion::jet4) {
        code_page = decode_jet4_code_page(file.read_page(0));
        if (!code_page) {
            errors.push_back({"file", std::nullopt, std::nullopt, "Jet4 root page code page is truncated", 0});
        }
    }

    TableSchemaCatalog schema;
    try {
        schema = read_table_schema_catalog(file);
        for (const auto& error : schema.errors) {
            errors.push_back({"table-schema", error.table_name, std::nullopt, error.message, error.page_number});
        }
    } catch (const DatabaseError& error) {
        errors.push_back({"table-schema", std::nullopt, std::nullopt, error.what(), error.page_number()});
    }

    output << "{\"canonicalVersion\":1,\"file\":{\"alias\":";
    write_json_string(output, alias);
    output << ",\"sourceSha256\":";
    write_optional_string(output, file_hash);
    output << ",\"byteCount\":";
    write_optional_number(output, stored_byte_count);
    output << ",\"jetVersion\":";
    write_json_string(output, file.format().name());
    output << ",\"codePage\":";
    write_optional_number(output, code_page);
    output << "},\"tables\":[";

    for (std::size_t index = 0; index < schema.tables.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        const auto& table = schema.tables[index];
        std::optional<SerializedTableRows> rows;
        try {
            rows = serialize_table_rows(file, table);
        } catch (const DatabaseError& error) {
            errors.push_back({"row-data", table.name, error.context().column_name,
                              error.what(), error.page_number(), error.context().row_number});
        }
        write_schema_table(output, table, rows ? &*rows : nullptr);
        if (!table.definition.columns.empty() && !table.definition.column_properties_available) {
            errors.push_back({"column-schema", table.name, std::nullopt,
                              "Column property maps are unavailable; nullable values are null", std::nullopt});
        }
        for (const auto& column : table.definition.columns) {
            if (!canonical_type(column.type)) {
                errors.push_back({"column-schema", table.name, column.name,
                                  "Canonical type mapping is not defined for this MDB type", std::nullopt});
            }
        }
    }

    output << "],\"errors\":[";
    for (std::size_t index = 0; index < errors.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        write_error(output, errors[index]);
    }
    output << "]}\n";
}

}
