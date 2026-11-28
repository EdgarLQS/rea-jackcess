#pragma once

#include "usage_map.hpp"
#include "../page/page_file.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

// Collation descriptor stored on a TEXT/MEMO column.
struct Jet4TextSortOrder {
    std::uint16_t lcid;  // Windows locale id selecting the collation.
    std::uint8_t version; // Sort-version selector (0 legacy, 1 general, 0xFF/-1 general 97).
};

// One column of a table definition, decoded from the table's definition pages.
struct ColumnDefinition {
    std::string name;                         // Column name.
    std::uint8_t type;                        // Stored Jet/ACE type code (e.g. 0x04 LONG, 0x10 TEXT).
    std::uint16_t column_number;              // 1-based column number in the definition.
    std::uint16_t variable_table_index;       // Index of this column in the variable-length column table.
    std::uint16_t column_id;                  // Column identifier used by indexes.
    std::uint8_t precision;                   // NUMERIC precision.
    std::uint8_t scale;                       // NUMERIC scale (fractional digits).
    std::uint8_t flags;                       // Column flags (bit 0x01 = fixed-length).
    std::uint8_t extended_flags;              // Extended column flags.
    std::uint16_t fixed_data_offset;          // Offset of a fixed column's data within the row.
    std::uint16_t length;                     // Declared storage length.
    std::optional<bool> nullable;             // From the Required property map; null when absent.
    std::optional<UsageMapReference> long_value_owned_pages;      // Usage map of LVAL pages this column owns.
    std::optional<UsageMapReference> long_value_free_space_pages; // Usage map of free space in those pages.
    std::optional<Jet4TextSortOrder> text_sort_order;            // TEXT/MEMO collation, when declared.
    std::optional<std::string> default_expression;               // DefaultValue expression text.
    std::optional<std::string> validation_rule;                  // Column ValidationRule text.
    std::optional<std::string> validation_text;                  // Column ValidationText.
    std::optional<std::uint32_t> complex_type_id;                // ACE complex-column type id.
    bool calculated = false;                                     // True for a calculated column.
    bool complex_value_foreign_key = false;                      // True for a flat-table complex FK column.
    std::optional<std::uint8_t> calculated_result_type;          // Result type code of a calculated column.
};

// One key column of a physical index, with its sort direction.
struct IndexColumnDefinition {
    std::uint16_t column_number; // 1-based column number in the table definition.
    bool ascending;              // true = ascending, false = descending.
};

// A physical (on-disk) B-tree index.
struct PhysicalIndexDefinition {
    std::vector<IndexColumnDefinition> columns; // Key columns in order.
    UsageMapReference owned_pages;              // Usage map of the pages the index owns.
    std::uint32_t root_page_number;             // Root page of the B-tree.
    std::uint32_t unique_entry_count;           // Number of distinct keys.
    std::uint8_t flags;                         // 0x01 unique, 0x02 ignore-nulls, 0x08 required.
    std::uint8_t extended_flags;                // Extended flags byte.
};

// A logical index (a named index that points at a physical index).
struct LogicalIndexDefinition {
    std::uint32_t index_number;                 // Logical index number.
    std::uint32_t index_data_number;            // Physical index number it points at (0-based).
    std::uint8_t referenced_index_type;         // For a foreign key: 1 = primary, 2 = foreign.
    std::int32_t referenced_index_number;       // Referenced parent-table index number (-1 if none).
    std::uint32_t referenced_table_page_number; // Referenced parent-table page.
    std::uint8_t cascade_updates_flags;         // Cascade-update action flags.
    std::uint8_t cascade_deletes_flags;         // Cascade-delete / set-null action flags.
    std::uint8_t type;                          // Index type (1 primary key, 2 foreign key, 0 user).
    std::string name;                           // Index name.
};

// A complete table definition parsed from its definition pages.
struct TableDefinition {
    std::uint32_t definition_size;              // Total size in bytes of the definition (incl. continuation pages).
    std::uint32_t row_count;                    // Header row count.
    std::uint8_t table_type;                    // Table type code (user vs system).
    std::uint16_t maximum_column_count;         // Max columns the definition can hold.
    std::uint16_t variable_column_count;        // Number of variable-length columns.
    std::uint16_t column_count;                 // Number of columns.
    std::uint32_t logical_index_count;          // Number of logical indexes.
    std::uint32_t physical_index_count;         // Number of physical indexes.
    std::uint64_t continuation_page_count;      // Number of continuation pages for the definition.
    UsageMapReference owned_pages;              // Usage map of the table's data pages.
    UsageMapReference free_space_pages;         // Usage map of free space across data pages.
    std::vector<ColumnDefinition> columns;      // Columns in definition order.
    std::vector<PhysicalIndexDefinition> physical_indexes; // Physical indexes (indexed by index_data_number).
    std::vector<LogicalIndexDefinition> indexes;           // Logical indexes.
    std::optional<std::string> table_validation_rule;      // Table-level ValidationRule.
    std::optional<std::string> table_validation_text;      // Table-level ValidationText.
    bool column_properties_available = false;              // True when column property maps were readable.
};

// Reads a table definition starting at `page_number`. `file` supplies the (possibly decoded) pages.
// Throws DatabaseError for a truncated, corrupt, or out-of-range definition.
TableDefinition read_table_definition(const PageFile& file, std::uint64_t page_number);

}
