#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

// Decodes single-byte code-page text. Windows-1252 matches the Jet3 samples; the five undefined
// byte values fall back to Latin-1 like most Access files. Other code pages need a real mapping
// table and are tracked in plan/Jackcess-C++重写/12-Jet3只读实施计划.md.
std::string decode_code_page_text(const std::vector<std::uint8_t>& bytes);

}  // namespace rea_jackcess::detail
