#include "system_catalog.hpp"

#include "../value/value_decode.hpp"
#include "../value/jet4_value.hpp"
#include "../util/code_page.hpp"

#include "property_map.hpp"
#include "row_reader.hpp"
#include "../format/jet_format.hpp"
#include "../value/jet4_value.hpp"
#include "../value/text_codec.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>
#include <utility>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint8_t short_type = 0x03;
constexpr std::uint8_t long_type = 0x04;
constexpr std::uint8_t binary_type = 0x09;
constexpr std::uint8_t text_type = 0x0a;
constexpr std::uint8_t ole_type = 0x0b;
constexpr std::uint8_t memo_type = 0x0c;
constexpr std::uint8_t big_binary_type = 0x11;

[[noreturn]] void corrupted_row(const PageFile& file,
                                std::uint64_t page_number,
                                const char* reason,
                                DatabaseErrorContext context = {}) {
    if (!context.table_name) {
        context.table_name = "MSysObjects";
    }
    throw DatabaseError(DatabaseErrorCode::corrupted_file,
                        reason,
                        file.path(),
                        page_number,
                        std::move(context));
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

bool equals_ignore_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    return std::equal(left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    });
}

const ColumnDefinition& find_column(const PageFile& file,
                                   std::uint64_t page_number,
                                   const TableDefinition& definition,
                                   std::string_view name,
                                   std::uint8_t type) {
    const auto column = std::find_if(definition.columns.begin(), definition.columns.end(), [&](const auto& candidate) {
        return equals_ignore_case(candidate.name, name);
    });
    if (column == definition.columns.end() || column->type != type) {
        DatabaseErrorContext context;
        context.table_name = "MSysObjects";
        context.column_name = std::string(name);
        corrupted_row(file,
                      page_number,
                      "System catalog is missing a required column or has an unsupported type",
                      std::move(context));
    }
    return *column;
}

const ColumnDefinition* find_optional_column(const TableDefinition& definition, std::string_view name) {
    const auto column = std::find_if(definition.columns.begin(), definition.columns.end(), [&](const auto& candidate) {
        return equals_ignore_case(candidate.name, name);
    });
    return column == definition.columns.end() ? nullptr : &*column;
}

const EncodedColumnValue& encoded_value(const PageFile& file,
                                        const EncodedTableRow& row,
                                        const ColumnDefinition& column) {
    if (column.column_number >= row.columns.size()) {
        DatabaseErrorContext context;
        context.table_name = "MSysObjects";
        context.row_number = row.row_number;
        context.column_number = column.column_number;
        context.column_name = column.name;
        corrupted_row(file, row.page_number, "System catalog row is missing a decoded column", std::move(context));
    }
    return row.columns[column.column_number];
}

std::optional<std::int32_t> read_int32(const PageFile& file,
                                       const EncodedTableRow& row,
                                       const ColumnDefinition& column) {
    const auto& encoded = encoded_value(file, row, column);
    if (!encoded.bytes) {
        return std::nullopt;
    }
    if (encoded.bytes->size() != 4) {
        DatabaseErrorContext context;
        context.table_name = "MSysObjects";
        context.row_number = row.row_number;
        context.column_number = column.column_number;
        context.column_name = column.name;
        corrupted_row(file, row.page_number,
                      "System catalog 32-bit integer has an invalid length", std::move(context));
    }
    const std::uint32_t value = read_u32(*encoded.bytes, 0);
    if (value <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return static_cast<std::int32_t>(value);
    }
    return static_cast<std::int32_t>(-1 - static_cast<std::int64_t>(~value));
}

std::optional<std::int16_t> read_int16(const PageFile& file,
                                       const EncodedTableRow& row,
                                       const ColumnDefinition& column) {
    const auto& encoded = encoded_value(file, row, column);
    if (!encoded.bytes) {
        return std::nullopt;
    }
    if (encoded.bytes->size() != 2) {
        DatabaseErrorContext context;
        context.table_name = "MSysObjects";
        context.row_number = row.row_number;
        context.column_number = column.column_number;
        context.column_name = column.name;
        corrupted_row(file, row.page_number,
                      "System catalog 16-bit integer has an invalid length", std::move(context));
    }
    const std::uint16_t value = read_u16(*encoded.bytes, 0);
    if (value <= static_cast<std::uint16_t>(std::numeric_limits<std::int16_t>::max())) {
        return static_cast<std::int16_t>(value);
    }
    return static_cast<std::int16_t>(-1 - static_cast<std::int32_t>(static_cast<std::uint16_t>(~value)));
}

std::optional<std::string> read_text(const PageFile& file,
                                     const EncodedTableRow& row,
                                     const ColumnDefinition& column) {
    const auto& encoded = encoded_value(file, row, column);
    if (!encoded.bytes) {
        return std::nullopt;
    }
    try {
        if (file.format().version == EngineVersion::jet3) {
            return decode_code_page_text(*encoded.bytes);
        }
        return decode_jet4_text(*encoded.bytes);
    } catch (const DatabaseError& error) {
        DatabaseErrorContext context;
        context.table_name = "MSysObjects";
        context.row_number = row.row_number;
        context.column_number = column.column_number;
        context.column_name = column.name;
        throw DatabaseError(error.code(), error.what(), file.path(), row.page_number, std::move(context));
    }
}

std::optional<std::vector<std::uint8_t>> read_property_bytes(const PageFile& file,
                                                             const EncodedTableRow& row,
                                                             const ColumnDefinition& column) {
    if (column.type != binary_type && column.type != ole_type &&
        column.type != memo_type && column.type != big_binary_type) {
        DatabaseErrorContext context;
        context.table_name = "MSysObjects";
        context.row_number = row.row_number;
        context.column_number = column.column_number;
        context.column_name = column.name;
        corrupted_row(file,
                      row.page_number,
                      "System catalog property column has an unsupported type",
                      std::move(context));
    }

    const auto value = decode_value(file, "MSysObjects", row, column);
    if (const auto* blob = std::get_if<BlobValue>(&value)) {
        return blob->bytes;
    }
    if (const auto* memo = std::get_if<MemoValue>(&value)) {
        return memo->raw_bytes;
    }
    if (std::holds_alternative<std::monostate>(value)) {
        return std::nullopt;
    }

    DatabaseErrorContext context;
    context.table_name = "MSysObjects";
    context.row_number = row.row_number;
    context.column_number = column.column_number;
    context.column_name = column.name;
    corrupted_row(file,
                  row.page_number,
                  "System catalog property column could not be decoded as bytes",
                  std::move(context));
}

}

std::vector<SystemCatalogObject> read_system_catalog_objects(const PageFile& file,
                                                              const TableDefinition& definition,
                                                              std::optional<std::string_view> property_object_name,
                                                              bool read_link_fields) {
    if (!has_jet4_page_layout(file.format().version) &&
        file.format().version != EngineVersion::jet3) {
        throw DatabaseError(DatabaseErrorCode::unsupported_version,
                            "System catalog layout requested for an unsupported engine version",
                            file.path());
    }

    const std::uint64_t context_page = definition.owned_pages.page_number;
    const auto& id_column = find_column(file, context_page, definition, "Id", long_type);
    const auto& parent_id_column = find_column(file, context_page, definition, "ParentId", long_type);
    const auto& name_column = find_column(file, context_page, definition, "Name", text_type);
    const auto& type_column = find_column(file, context_page, definition, "Type", short_type);
    const auto& flags_column = find_column(file, context_page, definition, "Flags", long_type);
    const auto* properties_column = find_optional_column(definition, "LvProp");
    const auto* link_database = read_link_fields ? find_optional_column(definition, "Database") : nullptr;
    const auto* link_table = read_link_fields ? find_optional_column(definition, "ForeignName") : nullptr;
    const auto* connection = read_link_fields ? find_optional_column(definition, "Connect") : nullptr;
    const auto rows = read_table_rows(file, "MSysObjects", definition);

    std::vector<SystemCatalogObject> objects;
    objects.reserve(rows.size());
    for (const auto& row : rows) {
        SystemCatalogObject object{
            read_int32(file, row, id_column),
            read_int32(file, row, parent_id_column),
            read_text(file, row, name_column),
            read_int16(file, row, type_column),
            read_int32(file, row, flags_column),
        };
        object.page_number = row.page_number;
        object.row_number = row.row_number;
        object.property_column_present = properties_column != nullptr;
        const bool read_properties = properties_column != nullptr &&
                                     (!property_object_name ||
                                      (object.name && equals_ignore_case(*object.name, *property_object_name)));
        if (read_properties) {
            try {
                object.property_bytes = read_property_bytes(file, row, *properties_column);
            } catch (const DatabaseError& error) {
                object.property_error = error.what();
                object.property_error_page = error.page_number();
            }
        }
        if (read_link_fields && (object.type == 4 || object.type == 6)) {
            const auto read_link_value = [&](const ColumnDefinition* column) -> std::optional<std::string> {
                if (!column) {
                    return std::nullopt;
                }
                const auto value = decode_value(file, "MSysObjects", row, *column);
                if (const auto* text = std::get_if<std::string>(&value)) {
                    return *text;
                }
                if (const auto* memo = std::get_if<MemoValue>(&value)) {
                    return memo->text;
                }
                if (std::holds_alternative<std::monostate>(value)) {
                    return std::nullopt;
                }
                DatabaseErrorContext context;
                context.table_name = "MSysObjects";
                context.row_number = row.row_number;
                context.column_name = column->name;
                corrupted_row(file, row.page_number, "Linked table field is not text", std::move(context));
            };
            object.linked_database = read_link_value(link_database);
            object.linked_table = read_link_value(link_table);
            object.connection_string = read_link_value(connection);
        }
        objects.push_back(std::move(object));
    }
    return objects;
}

FileFormat identify_file_format(const PageFile& file) {
    switch (file.format().version) {
        case EngineVersion::jet3: return FileFormat::v1997;
        case EngineVersion::ace2007: return FileFormat::v2007;
        case EngineVersion::ace2010: return FileFormat::v2010;
        case EngineVersion::ace2016: return FileFormat::v2016;
        case EngineVersion::ace2019: return FileFormat::v2019;
        case EngineVersion::msisam: return FileFormat::msisam;
        case EngineVersion::jet4: break;
    }

    const auto catalog = read_table_definition(file, 2);
    const auto objects = read_system_catalog_objects(file, catalog, "MSysDb");
    const auto object = std::find_if(objects.begin(), objects.end(), [](const auto& item) {
        return item.name == "MSysDb" && item.type == 2;
    });
    if (object == objects.end()) {
        return FileFormat::generic_jet4;
    }
    DatabaseErrorContext context;
    context.table_name = "MSysDb";
    context.row_number = object->row_number;
    context.column_name = "LvProp";
    if (object->property_error) {
        throw DatabaseError(DatabaseErrorCode::corrupted_file, *object->property_error, file.path(),
                            object->property_error_page, context);
    }
    if (!object->property_bytes) {
        return FileFormat::generic_jet4;
    }
    const auto access = read_text_property(file, "MSysDb", object->page_number, object->row_number,
                                           *object->property_bytes, "AccessVersion");
    if (!access || access->find_first_not_of(" \t\r\n\f\v") == std::string::npos) {
        return FileFormat::generic_jet4;
    }
    if (*access == "08.50") {
        return FileFormat::v2000;
    }
    if (*access == "09.50") {
        return FileFormat::v2003;
    }
    throw DatabaseError(DatabaseErrorCode::unsupported_version, "Unrecognized Jet4 AccessVersion",
                        file.path(), object->page_number, context);
}

}
