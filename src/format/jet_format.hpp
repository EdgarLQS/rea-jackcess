#pragma once

#include "rea_jackcess/database_file.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace rea_jackcess::detail {

constexpr std::size_t database_header_size = 21;

FormatInfo detect_format(const std::array<std::uint8_t, database_header_size>& header,
                         const std::filesystem::path& path);
std::optional<std::uint16_t> decode_jet4_code_page(const std::vector<std::uint8_t>& raw_root_page);

}
