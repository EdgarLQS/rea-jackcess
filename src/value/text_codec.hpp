#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

std::string decode_utf16le(const std::vector<std::uint8_t>& bytes);
std::string decode_jet4_text(const std::vector<std::uint8_t>& bytes);

}
