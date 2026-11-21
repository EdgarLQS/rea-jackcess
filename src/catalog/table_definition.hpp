#pragma once

#include "usage_map.hpp"
#include "../page/page_file.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

struct ColumnDefinition {
    std::string name;
    std::uint8_t type;
    std::uint16_t column_number;
    std::uint16_t variable_table_index;
    std::uint16_t column_id;
    std::uint8_t precision;
    std::uint8_t scale;
    std::uint8_t flags;
    std::uint8_t extended_flags;
    std::uint16_t fixed_data_offset;
    std::uint16_t length;
    std::optional<bool> nullable;
    std::optional<UsageMapReference> long_value_owned_pages;
    std::optional<UsageMapReference> long_value_free_space_pages;
};

struct IndexColumnDefinition {
    std::uint16_t column_number;
    bool ascending;
};

struct PhysicalIndexDefinition {
    std::vector<IndexColumnDefinition> columns;
    UsageMapReference owned_pages;
    std::uint32_t root_page_number;
    std::uint32_t unique_entry_count;
    std::uint8_t flags;
    std::uint8_t extended_flags;
};

struct LogicalIndexDefinition {
    std::uint32_t index_number;
    std::uint32_t index_data_number;
    std::uint8_t referenced_index_type;
    std::int32_t referenced_index_number;
    std::uint32_t referenced_table_page_number;
    std::uint8_t cascade_updates_flags;
    std::uint8_t cascade_deletes_flags;
    std::uint8_t type;
    std::string name;
};

struct TableDefinition {
    std::uint32_t definition_size;
    std::uint32_t row_count;
    std::uint8_t table_type;
    std::uint16_t maximum_column_count;
    std::uint16_t variable_column_count;
    std::uint16_t column_count;
    std::uint32_t logical_index_count;
    std::uint32_t physical_index_count;
    std::uint64_t continuation_page_count;
    UsageMapReference owned_pages;
    UsageMapReference free_space_pages;
    std::vector<ColumnDefinition> columns;
    std::vector<PhysicalIndexDefinition> physical_indexes;
    std::vector<LogicalIndexDefinition> indexes;
    bool column_properties_available = false;
};

TableDefinition read_table_definition(const PageFile& file, std::uint64_t page_number);

}
