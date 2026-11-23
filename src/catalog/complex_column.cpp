#include "complex_column.hpp"

#include "row_reader.hpp"
#include "../value/jet4_value.hpp"
#include "../value/unicode_case.hpp"

#include <algorithm>
#include <iterator>
#include <string_view>
#include <utility>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint8_t byte_type = 0x02;
constexpr std::uint8_t long_type = 0x04;
constexpr std::uint8_t single_type = 0x06;
constexpr std::uint8_t double_type = 0x07;
constexpr std::uint8_t short_date_time_type = 0x08;
constexpr std::uint8_t text_type = 0x0a;
constexpr std::uint8_t ole_type = 0x0b;
constexpr std::uint8_t memo_type = 0x0c;
constexpr std::uint8_t guid_type = 0x0f;
constexpr std::uint8_t numeric_type = 0x10;
constexpr std::uint8_t int64_type = 0x13;
constexpr std::uint8_t auto_number_flag = 0x04;
constexpr std::uint8_t auto_number_guid_flag = 0x40;

bool is_ace(EngineVersion version) {
    switch (version) {
        case EngineVersion::ace2007:
        case EngineVersion::ace2010:
        case EngineVersion::ace2016:
        case EngineVersion::ace2019:
            return true;
        case EngineVersion::jet3:
        case EngineVersion::jet4:
        case EngineVersion::msisam:
            return false;
    }
    return false;
}

bool string_case_equal(std::string_view left, std::string_view right) {
    return java_string_equals_ignore_case_utf8(left, right);
}

const ColumnDefinition* find_column(const TableDefinition& definition, std::string_view name) {
    const auto column = std::find_if(definition.columns.begin(), definition.columns.end(), [&](const auto& candidate) {
        return string_case_equal(candidate.name, name);
    });
    return column == definition.columns.end() ? nullptr : &*column;
}

std::optional<std::uint32_t> positive_int32(const Value& value) {
    const auto* integer = std::get_if<std::int32_t>(&value);
    if (integer == nullptr || *integer <= 0) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*integer);
}

std::optional<std::string> text_value(const Value& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    return std::nullopt;
}

bool string_case_starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && string_case_equal(value.substr(0, prefix.size()), prefix);
}

std::size_t count_column_type(const TableDefinition& definition, std::uint8_t type) {
    return static_cast<std::size_t>(std::count_if(definition.columns.begin(), definition.columns.end(), [&](const auto& column) {
        return column.type == type;
    }));
}

bool is_multi_value_type_table(const TableDefinition& definition) {
    if (definition.columns.size() != 1) {
        return false;
    }

    switch (definition.columns.front().type) {
        case byte_type:
        case long_type:
        case single_type:
        case double_type:
        case guid_type:
        case numeric_type:
        case text_type:
        case int64_type:
            return true;
        default:
            return false;
    }
}

bool is_attachment_type_table(const TableDefinition& definition) {
    return definition.columns.size() >= 6 &&
           count_column_type(definition, memo_type) >= 1 &&
           count_column_type(definition, text_type) >= 2 &&
           count_column_type(definition, ole_type) >= 1 &&
           count_column_type(definition, short_date_time_type) >= 1 &&
           count_column_type(definition, long_type) >= 1;
}

bool is_version_history_type_table(const TableDefinition& definition) {
    return definition.columns.size() >= 2 &&
           count_column_type(definition, memo_type) >= 1 &&
           count_column_type(definition, short_date_time_type) >= 1;
}

ComplexColumnKind classify_complex_column_type(std::string_view type_table_name,
                                               const TableDefinition& type_table_definition) {
    constexpr std::string_view attachment_name = "MSysComplexType_Attachment";
    constexpr std::string_view version_history_prefix = "MSysComplexTypeVH_";
    constexpr std::string_view complex_type_prefix = "MSysComplexType_";

    if (string_case_equal(type_table_name, attachment_name)) {
        return ComplexColumnKind::attachment;
    }
    if (string_case_starts_with(type_table_name, version_history_prefix)) {
        return ComplexColumnKind::version_history;
    }
    if (string_case_starts_with(type_table_name, complex_type_prefix)) {
        return is_multi_value_type_table(type_table_definition)
                   ? ComplexColumnKind::multi_value
                   : ComplexColumnKind::unsupported;
    }
    if (is_multi_value_type_table(type_table_definition)) {
        return ComplexColumnKind::multi_value;
    }
    if (is_attachment_type_table(type_table_definition)) {
        return ComplexColumnKind::attachment;
    }
    if (is_version_history_type_table(type_table_definition)) {
        return ComplexColumnKind::version_history;
    }
    return ComplexColumnKind::unsupported;
}

bool is_auto_number(const ColumnDefinition& column) {
    return (column.flags & (auto_number_flag | auto_number_guid_flag)) != 0;
}

bool is_type_column(const TableDefinition& type_table, const ColumnDefinition& flat_column) {
    return std::any_of(type_table.columns.begin(), type_table.columns.end(), [&](const auto& type_column) {
        return string_case_equal(type_column.name, flat_column.name);
    });
}

std::optional<std::size_t> table_index_for_page(const TableSchemaCatalog& schema, std::uint32_t page_number) {
    const auto table = std::find_if(schema.tables.begin(), schema.tables.end(), [&](const auto& item) {
        return item.definition_page_number == page_number;
    });
    if (table == schema.tables.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(schema.tables.begin(), table));
}

void add_error(ComplexColumnCatalog& catalog,
               const std::string& table_name,
               const std::string& message,
               std::optional<std::uint64_t> page_number = std::nullopt) {
    catalog.errors.push_back({table_name, message, page_number});
}

}

ComplexFlatTableKeys find_complex_flat_table_keys(const TableDefinition& type_table,
                                                   const TableDefinition& flat_table) {
    ComplexFlatTableKeys keys;
    for (const auto& column : flat_table.columns) {
        if (is_type_column(type_table, column)) {
            continue;
        }
        if (column.complex_value_foreign_key) {
            keys.foreign_key_column_name = column.name;
        } else if (is_auto_number(column)) {
            keys.primary_key_column_name = column.name;
        }
    }

    if (!keys.foreign_key_column_name) {
        // Jackcess 5.2.0 keeps the last qualifying LONG column when the ACE flag is absent.
        for (const auto& column : flat_table.columns) {
            if (!is_type_column(type_table, column) && !is_auto_number(column) && column.type == long_type) {
                keys.foreign_key_column_name = column.name;
            }
        }
    }
    return keys;
}

ComplexColumnCatalog read_complex_column_catalog(const PageFile& file, const TableSchemaCatalog& schema) {
    ComplexColumnCatalog result;
    if (!is_ace(file.format().version)) {
        return result;
    }

    const auto catalog_table = std::find_if(schema.tables.begin(), schema.tables.end(), [](const auto& table) {
        return string_case_equal(table.name, "MSysComplexColumns");
    });
    if (catalog_table == schema.tables.end()) {
        return result;
    }

    const auto& definition = catalog_table->definition;
    const auto* complex_id_column = find_column(definition, "ComplexID");
    const auto* column_name_column = find_column(definition, "ColumnName");
    const auto* conceptual_table_column = find_column(definition, "ConceptualTableID");
    const auto* type_table_column = find_column(definition, "ComplexTypeObjectID");
    const auto* flat_table_column = find_column(definition, "FlatTableID");
    if (complex_id_column == nullptr || conceptual_table_column == nullptr ||
        type_table_column == nullptr || flat_table_column == nullptr) {
        add_error(result,
                  catalog_table->name,
                  "MSysComplexColumns does not contain the required association fields",
                  catalog_table->definition_page_number);
        return result;
    }

    std::vector<EncodedTableRow> rows;
    try {
        rows = read_table_rows(file, catalog_table->name, definition);
    } catch (const DatabaseError& error) {
        add_error(result, catalog_table->name, error.what(), error.page_number());
        return result;
    }

    for (const auto& row : rows) {
        try {
            const auto decode = [&](const ColumnDefinition& column) {
                return decode_jet4_value(file, catalog_table->name, row, column);
            };
            const auto complex_id = positive_int32(decode(*complex_id_column));
            const auto conceptual_page = positive_int32(decode(*conceptual_table_column));
            const auto type_page = positive_int32(decode(*type_table_column));
            const auto flat_page = positive_int32(decode(*flat_table_column));
            const auto column_name = column_name_column == nullptr
                                         ? std::optional<std::string>{}
                                         : text_value(decode(*column_name_column));
            if (!complex_id || !conceptual_page || !type_page || !flat_page) {
                add_error(result,
                          catalog_table->name,
                          "Complex-column association row has a missing or invalid page/id field",
                          row.page_number);
                continue;
            }

            const auto conceptual_index = table_index_for_page(schema, *conceptual_page);
            if (!conceptual_index) {
                add_error(result,
                          catalog_table->name,
                          "Complex-column association references an unknown conceptual table page",
                          row.page_number);
                continue;
            }
            const auto& conceptual_table = schema.tables[*conceptual_index];
            const auto complex_column = std::find_if(
                conceptual_table.definition.columns.begin(), conceptual_table.definition.columns.end(),
                [&](const auto& candidate) {
                    return candidate.complex_type_id == complex_id &&
                           (!column_name || string_case_equal(candidate.name, *column_name));
                });
            if (complex_column == conceptual_table.definition.columns.end()) {
                add_error(result,
                          catalog_table->name,
                          "Complex-column association does not match a column definition",
                          row.page_number);
                continue;
            }

            const auto type_index = table_index_for_page(schema, *type_page);
            const auto flat_index = table_index_for_page(schema, *flat_page);
            ComplexColumnAssociation association{
                *conceptual_index,
                static_cast<std::size_t>(std::distance(conceptual_table.definition.columns.begin(), complex_column)),
                *conceptual_page,
                *type_page,
                type_index ? std::optional<std::string>{schema.tables[*type_index].name} : std::nullopt,
                type_index ? std::optional<ComplexColumnKind>{classify_complex_column_type(
                                 schema.tables[*type_index].name,
                                 schema.tables[*type_index].definition)}
                           : std::nullopt,
                *flat_page,
                flat_index ? std::optional<std::string>{schema.tables[*flat_index].name} : std::nullopt,
                std::nullopt,
                std::nullopt,
            };

            if (!type_index || !flat_index) {
                add_error(result,
                          catalog_table->name,
                          "Complex-column association references a missing type or flat table",
                          row.page_number);
            } else {
                const auto keys = find_complex_flat_table_keys(schema.tables[*type_index].definition,
                                                                 schema.tables[*flat_index].definition);
                association.primary_key_column_name = keys.primary_key_column_name;
                association.foreign_key_column_name = keys.foreign_key_column_name;

                if (!association.primary_key_column_name || !association.foreign_key_column_name) {
                    add_error(result,
                              catalog_table->name,
                              "Could not find the expected AutoNumber primary key and complex-value foreign key in flat table",
                              *flat_page);
                }
            }

            result.associations.push_back(std::move(association));
        } catch (const DatabaseError& error) {
            add_error(result, catalog_table->name, error.what(), row.page_number);
        }
    }
    return result;
}

}
