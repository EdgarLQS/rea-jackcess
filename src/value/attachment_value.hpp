#pragma once

#include <cstdint>
#include <vector>

namespace rea_jackcess::detail {

std::vector<std::uint8_t> decode_attachment_file_data(const std::vector<std::uint8_t>& encoded_data);

}
