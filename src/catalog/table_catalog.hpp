#pragma once

#include "table_definition.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

struct ReadableTableSchema {
    std::string name;
    std::string object_type;
    bool is_user_table;
    bool is_linked;
    TableDefinition definition;
};

struct TableSchemaError {
    std::string table_name;
    std::string message;
    std::optional<std::uint64_t> page_number;
};

struct TableSchemaCatalog {
    std::vector<ReadableTableSchema> tables;
    std::vector<TableSchemaError> errors;
};

TableSchemaCatalog read_table_schema_catalog(const PageFile& file);

}
