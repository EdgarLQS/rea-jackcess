#pragma once

#include "table_catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

// The primary-key and foreign-key column names discovered on a complex column's flat table.
struct ComplexFlatTableKeys {
    std::optional<std::string> primary_key_column_name; // Flat table's AutoNumber key column.
    std::optional<std::string> foreign_key_column_name;  // Flat table's column tying rows to the parent.
};

// Internal classification of a complex column (mirrors the public ComplexColumnType).
enum class ComplexColumnKind {
    multi_value,
    attachment,
    version_history,
    unsupported,
};

// Links one complex column (identified by its position in the conceptual table) to its type and
// flat tables.
struct ComplexColumnAssociation {
    std::size_t conceptual_table_index;          // Index of the owning table in the catalog.
    std::size_t complex_column_index;            // Index of the column within that table.
    std::uint32_t conceptual_table_page_number;  // Definition page of the owning table.
    std::uint32_t complex_type_table_page_number; // Page of the complex-type metadata table.
    std::optional<std::string> complex_type_table_name; // Name of that table, if present.
    std::optional<ComplexColumnKind> type;       // Classified kind, when derivable.
    std::uint32_t flat_table_page_number;        // Page of the flat value table.
    std::optional<std::string> flat_table_name;  // Name of the flat table, if present.
    std::optional<std::string> primary_key_column_name; // Flat table key column.
    std::optional<std::string> foreign_key_column_name; // Flat table parent-link column.
};

// All complex-column associations plus the per-column errors found while building them.
struct ComplexColumnCatalog {
    std::vector<ComplexColumnAssociation> associations; // Resolved complex columns.
    std::vector<TableSchemaError> errors;               // Columns that could not be resolved.
};

// Identifies the key column names of a flat table from its type table.
ComplexFlatTableKeys find_complex_flat_table_keys(const TableDefinition& type_table,
                                                   const TableDefinition& flat_table);

// Resolves every ACE complex column in `schema` to its type/flat tables (reads MSysComplexColumns
// and the referenced tables). Records unresolvable columns in the result's `errors` rather than
// throwing.
ComplexColumnCatalog read_complex_column_catalog(const PageFile& file, const TableSchemaCatalog& schema);

}
