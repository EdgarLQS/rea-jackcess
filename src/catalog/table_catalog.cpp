#include "table_catalog.hpp"

#include "property_map.hpp"
#include "system_catalog.hpp"

#include <utility>

namespace rea_jackcess::detail {
namespace {

constexpr std::int16_t table_object_type = 1;
constexpr std::int16_t linked_odbc_object_type = 4;
constexpr std::int16_t linked_table_object_type = 6;
constexpr std::uint32_t system_object_flags = 0x80000002u;
constexpr std::uint8_t system_table_type = 0x53;
constexpr std::uint8_t user_table_type = 0x4e;

void add_error(TableSchemaCatalog& catalog,
               const std::string& name,
               const std::string& message,
               std::optional<std::uint64_t> page_number = std::nullopt) {
    catalog.errors.push_back({name, message, page_number});
}

}

TableSchemaCatalog read_table_schema_catalog(const PageFile& file) {
    if (file.format().version != EngineVersion::jet4) {
        throw DatabaseError(DatabaseErrorCode::unsupported_version,
                            "Jet4 table schema enumeration requested for a different engine version",
                            file.path());
    }

    const auto system_catalog = read_table_definition(file, 2);
    const auto objects = read_system_catalog_objects(file, system_catalog);
    TableSchemaCatalog result;
    for (const auto& object : objects) {
        if (!object.type || (*object.type != table_object_type &&
                             *object.type != linked_odbc_object_type &&
                             *object.type != linked_table_object_type)) {
            continue;
        }
        const std::string name = object.name.value_or("<unnamed>");
        if (*object.type == linked_odbc_object_type || *object.type == linked_table_object_type) {
            add_error(result, name, "Linked table schema is not available in the local database file");
            continue;
        }
        if (!object.id || *object.id <= 0 || static_cast<std::uint64_t>(*object.id) >= file.page_count()) {
            add_error(result, name, "Table definition page is missing or outside the database", 2);
            continue;
        }

        try {
            auto definition = read_table_definition(file, static_cast<std::uint64_t>(*object.id));
            if (object.property_column_present) {
                definition.column_properties_available = true;
                if (object.property_error) {
                    add_error(result, name, *object.property_error, object.property_error_page);
                } else {
                    std::vector<std::string> column_names;
                    column_names.reserve(definition.columns.size());
                    for (const auto& column : definition.columns) {
                        column_names.push_back(column.name);
                    }
                    try {
                        const std::vector<bool> nullable = read_column_nullability(
                            file,
                            name,
                            object.page_number,
                            object.row_number,
                            column_names,
                            object.property_bytes.value_or(std::vector<std::uint8_t>{}));
                        for (std::size_t index = 0; index < definition.columns.size(); ++index) {
                            definition.columns[index].nullable = nullable[index];
                        }
                    } catch (const DatabaseError& error) {
                        add_error(result, name, error.what(), error.page_number());
                    }
                }
            }
            const bool is_system_object = object.flags &&
                                          (static_cast<std::uint32_t>(*object.flags) & system_object_flags) != 0;
            const bool is_user_table = definition.table_type == user_table_type && !is_system_object;
            std::string object_type = "TABLE";
            if (definition.table_type == system_table_type || is_system_object) {
                object_type = "SYSTEM_TABLE";
            } else if (!is_user_table) {
                add_error(result, name, "Table definition has an unknown table type", *object.id);
            }
            result.tables.push_back({
                name,
                static_cast<std::uint64_t>(*object.id),
                std::move(object_type),
                is_user_table,
                false,
                std::move(definition),
            });
        } catch (const DatabaseError& error) {
            add_error(result, name, error.what(), error.page_number());
        }
    }
    return result;
}

}
