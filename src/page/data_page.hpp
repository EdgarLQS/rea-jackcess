#pragma once

#include "page_file.hpp"

#include <cstdint>
#include <vector>

namespace rea_jackcess::detail {

struct DataPageRow {
    std::uint16_t row_number;
    std::uint16_t start_offset;
    std::uint16_t end_offset;
    bool deleted;
    bool overflow;
    std::vector<std::uint8_t> bytes;
};

struct DataPage {
    std::uint64_t page_number;
    std::vector<DataPageRow> rows;
};

DataPage parse_data_page(const PageFile& file,
                         std::uint64_t page_number,
                         const std::vector<std::uint8_t>& bytes);
DataPage read_data_page(const PageFile& file, std::uint64_t page_number);

}
