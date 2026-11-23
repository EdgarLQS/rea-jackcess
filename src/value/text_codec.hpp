#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rea_jackcess::detail {

std::u16string utf8_to_utf16(std::string_view value,
                            std::size_t maximum_code_units,
                            const char* size_error);
std::string decode_utf16le(const std::vector<std::uint8_t>& bytes);
std::string decode_jet4_text(const std::vector<std::uint8_t>& bytes);

}
