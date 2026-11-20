#pragma once

#include "../page/page_file.hpp"

#include <cstdint>
#include <vector>

namespace rea_jackcess::detail {

std::vector<std::uint8_t> mark_global_pages_allocated(
    const PageFile& file,
    const std::vector<std::uint64_t>& page_numbers);

}
