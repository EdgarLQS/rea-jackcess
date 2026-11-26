#include "code_page.hpp"

#include <cstdint>

namespace rea_jackcess::detail {

std::string decode_code_page_text(const std::vector<std::uint8_t>& bytes) {
    static const std::uint16_t cp1252_high[32] = {
        0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
        0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
        0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
        0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
    };
    std::string result;
    result.reserve(bytes.size());
    for (const std::uint8_t byte : bytes) {
        char32_t code_point = byte;
        if (byte >= 0x80 && byte <= 0x9f) {
            code_point = cp1252_high[byte - 0x80];
        }
        if (code_point < 0x80) {
            result.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800) {
            result.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
            result.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
        } else {
            result.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
            result.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
        }
    }
    return result;
}

}  // namespace rea_jackcess::detail
