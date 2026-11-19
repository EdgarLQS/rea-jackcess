#pragma once

#include "../page/page_file.hpp"

#include <cstdint>
#include <vector>

namespace rea_jackcess::detail {

struct UsageMapReference {
    std::uint8_t row_number;
    std::uint32_t page_number;
};

std::vector<std::uint64_t> read_usage_map_pages(const PageFile& file, const UsageMapReference& reference);

}
