#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

// Decodes legacy code-page text. 1252 matches the Jet3 samples; 819/28591 are Latin-1; 1251,
// 936 (GBK) and 932 (Shift-JIS) replay the Java NIO decode results recorded in
// src/util/code_page_*.inc, so malformed sequences behave exactly like the fixed Jackcess running
// on that charset (one U+FFFD per malformed run). `code_page` defaults to 1252; other values
// throw std::invalid_argument, and callers validate the option before opening.
std::string decode_code_page_text(const std::vector<std::uint8_t>& bytes,
                                  std::uint16_t code_page = 1252);

}  // namespace rea_jackcess::detail
