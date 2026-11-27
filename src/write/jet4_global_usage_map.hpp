#pragma once

#include "../page/page_file.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace rea_jackcess::detail {

// Pending page-image updates to the database's global usage map, keyed by page number.
using GlobalUsagePageUpdates = std::map<std::uint64_t, std::vector<std::uint8_t>>;

class PageFileWriter;

// Marks `page_numbers` as allocated in the global usage map, recording changes in `updates`.
// Returns any new bitmap pages, which append after the caller's data or index pages.
std::vector<std::uint64_t> mark_global_pages_allocated(
    const PageFile& file, GlobalUsagePageUpdates& updates,
    const std::vector<std::uint64_t>& page_numbers);

// Writes the bitmap pages produced by mark_global_pages_allocated (in `appended` order).
void append_usage_bitmap_pages(PageFileWriter& writer, const GlobalUsagePageUpdates& updates,
                               const std::vector<std::uint64_t>& appended);

// Marks `page_numbers` as free in the global usage map, recording changes in `updates`.
// The global usage map stores 1 for a free page.
void mark_global_pages_free(const PageFile& file, GlobalUsagePageUpdates& updates,
                            const std::vector<std::uint64_t>& page_numbers);

}
