#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

// Decodes single-byte code-page text. Windows-1252 matches the Jet3 samples; the five undefined
// byte values fall back to Latin-1 like most Access files. Other code pages need a real mapping
// table and are tracked in plan/Jackcess-C++重写/12-Jet3只读实施计划.md.
// `code_page` is 1252 (default), or 819/28591 for Latin-1. Other values throw
// std::invalid_argument; callers validate the option before opening.
std::string decode_code_page_text(const std::vector<std::uint8_t>& bytes,
                                  std::uint16_t code_page = 1252);

}  // namespace rea_jackcess::detail
