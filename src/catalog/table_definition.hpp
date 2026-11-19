#pragma once

#include "../page/page_file.hpp"

#include <cstdint>
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
    std::vector<ColumnDefinition> columns;
};

TableDefinition read_table_definition(const PageFile& file, std::uint64_t page_number);

}
