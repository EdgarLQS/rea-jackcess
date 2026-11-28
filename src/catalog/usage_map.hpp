#pragma once

#include "../page/page_file.hpp"

#include <cstdint>
#include <vector>

namespace rea_jackcess::detail {

// A reference to a usage map: a bitmap that lives somewhere inside a page, used to track which
// pages a table/index/column owns or which slots inside a long-value page are free. It holds only
// the starting position; the bitmap may continue on further pages.
struct UsageMapReference {
    // Zero-based slot within `page_number` where the usage map starts.
    std::uint8_t row_number;
    // Page number holding the usage map's start.
    std::uint32_t page_number;
};

// Resolves a usage map reference to the list of pages marked "in use", following any continuation
// pages. Throws DatabaseError on a malformed or out-of-range map.
std::vector<std::uint64_t> read_usage_map_pages(const PageFile& file, const UsageMapReference& reference);

}
