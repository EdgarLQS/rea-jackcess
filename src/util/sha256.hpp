#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

std::string sha256_hex(const std::vector<std::uint8_t>& bytes);
std::string sha256_file_hex(const std::filesystem::path& path);

}
