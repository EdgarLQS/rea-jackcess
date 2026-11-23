#pragma once

#include "table_catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

struct ComplexFlatTableKeys {
    std::optional<std::string> primary_key_column_name;
    std::optional<std::string> foreign_key_column_name;
};

enum class ComplexColumnKind {
    multi_value,
    attachment,
    version_history,
    unsupported,
};

struct ComplexColumnAssociation {
    std::size_t conceptual_table_index;
    std::size_t complex_column_index;
    std::uint32_t conceptual_table_page_number;
    std::uint32_t complex_type_table_page_number;
    std::optional<std::string> complex_type_table_name;
    std::optional<ComplexColumnKind> type;
    std::uint32_t flat_table_page_number;
    std::optional<std::string> flat_table_name;
    std::optional<std::string> primary_key_column_name;
    std::optional<std::string> foreign_key_column_name;
};

struct ComplexColumnCatalog {
    std::vector<ComplexColumnAssociation> associations;
    std::vector<TableSchemaError> errors;
};

ComplexFlatTableKeys find_complex_flat_table_keys(const TableDefinition& type_table,
                                                   const TableDefinition& flat_table);

ComplexColumnCatalog read_complex_column_catalog(const PageFile& file, const TableSchemaCatalog& schema);

}
