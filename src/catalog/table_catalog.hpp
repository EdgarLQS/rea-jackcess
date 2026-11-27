#pragma once

#include "table_definition.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

// One table discovered in the catalog, together with its parsed definition.
struct ReadableTableSchema {
    std::string name;                         // Table name.
    std::uint64_t definition_page_number;     // Page holding the table definition.
    std::string object_type;                  // MSysObjects type text (e.g. "Table", "Query").
    bool is_user_table;                       // True for a user (not system) table.
    bool is_linked;                           // True for a linked (external) table.
    // MSysObjects carries a hidden flag (0x08) for objects Access hides from the UI.
    bool is_hidden = false;
    TableDefinition definition;               // Parsed table definition.
};

// A table whose definition could not be read; recorded so opening can report it as a diagnostic.
struct TableSchemaError {
    std::string table_name;                   // Table that failed.
    std::string message;                      // Why it failed.
    std::optional<std::uint64_t> page_number; // Definition page, when known.
};

// All readable table schemas plus the per-table failures encountered while loading them.
struct TableSchemaCatalog {
    std::vector<ReadableTableSchema> tables;  // Successfully read tables.
    std::vector<TableSchemaError> errors;     // Tables that could not be read.
};

// Scans MSysObjects and reads every table definition; never throws for a single bad table (it is
// recorded in `errors`).
TableSchemaCatalog read_table_schema_catalog(const PageFile& file);

}
