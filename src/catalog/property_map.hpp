#pragma once

#include "../page/page_file.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

std::vector<bool> read_column_nullability(const PageFile& file,
                                         const std::string& table_name,
                                         std::uint64_t page_number,
                                         std::uint16_t system_row_number,
                                         const std::vector<std::string>& column_names,
                                         const std::vector<std::uint8_t>& property_bytes);

}
