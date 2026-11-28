#pragma once

#include "page_file.hpp"

#include <cstdint>
#include <vector>

namespace rea_jackcess::detail {

// One row slot of a Jet4 data page, as located by the page's row directory.
struct DataPageRow {
    std::uint16_t row_number;  // Slot number within the page.
    std::uint16_t start_offset; // Offset of the row's data within the page.
    std::uint16_t end_offset;   // Offset just past the row's data within the page.
    bool deleted;               // Row is marked deleted.
    bool overflow;              // Row size is stored in the overflow table (extended row).
    std::vector<std::uint8_t> bytes; // The row's stored bytes (with any overflow bytes appended).
};

// A parsed data page and its live rows.
struct DataPage {
    std::uint64_t page_number; // The page number this was read from.
    std::vector<DataPageRow> rows; // The page's rows in directory order.
};

// Parses already-read `bytes` of `page_number` as a data page. `file` supplies page count for bounds.
DataPage parse_data_page(const PageFile& file,
                         std::uint64_t page_number,
                         const std::vector<std::uint8_t>& bytes);
// Reads and parses the data page at `page_number`.
DataPage read_data_page(const PageFile& file, std::uint64_t page_number);

}
