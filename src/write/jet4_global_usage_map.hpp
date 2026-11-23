#pragma once

#include "../page/page_file.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace rea_jackcess::detail {

using GlobalUsagePageUpdates = std::map<std::uint64_t, std::vector<std::uint8_t>>;

class PageFileWriter;

// Returns any new bitmap pages, which append after the caller's data or index pages.
std::vector<std::uint64_t> mark_global_pages_allocated(
    const PageFile& file, GlobalUsagePageUpdates& updates,
    const std::vector<std::uint64_t>& page_numbers);

void append_usage_bitmap_pages(PageFileWriter& writer, const GlobalUsagePageUpdates& updates,
                               const std::vector<std::uint64_t>& appended);

// The global usage map stores 1 for a free page.
void mark_global_pages_free(const PageFile& file, GlobalUsagePageUpdates& updates,
                            const std::vector<std::uint64_t>& page_numbers);

}
