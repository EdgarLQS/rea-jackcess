#pragma once

#include <cstdint>

namespace rea_jackcess::detail {

// Page type byte stored at the start of every Jet/ACE page (the low byte of the page header).
enum class PageType : std::uint8_t {
    invalid = 0x00,          // Not a recognised page.
    data = 0x01,             // Table data page (row directory + row data).
    table_definition = 0x02, // Table definition page (columns, index metadata).
    index_node = 0x03,       // Interior B-tree node of an index.
    index_leaf = 0x04,       // Leaf page of an index.
    usage_map = 0x05,        // Usage-map (bitmap) page.
};

}
