#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rea_jackcess::detail {

// Converts UTF-8 `value` to UTF-16 code units. `maximum_code_units` caps the result; if exceeded,
// throws std::invalid_argument using `size_error` as the message. Invalid UTF-8 also throws
// std::invalid_argument. Used to size and validate TEXT writes.
std::u16string utf8_to_utf16(std::string_view value,
                            std::size_t maximum_code_units,
                            const char* size_error);
// Decodes little-endian UTF-16 bytes into a UTF-8 std::string.
std::string decode_utf16le(const std::vector<std::uint8_t>& bytes);
// Decodes a Jet4 text column's stored bytes (UTF-16LE) into UTF-8.
std::string decode_jet4_text(const std::vector<std::uint8_t>& bytes);

}
