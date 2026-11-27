#include "code_page.hpp"

#include "../util/code_page_1251.inc"
#include "../util/code_page_932.inc"
#include "../util/code_page_gbk.inc"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint16_t replacement = 0xFFFD;
// See src/util/code_page_*.inc: 0xFFFD emits a lone U+FFFD, 0xFFFE emits U+FFFD plus the trail
// byte's own single-byte decode.
constexpr std::uint16_t replace_then_trail = 0xFFFE;
constexpr std::uint8_t trail_min = 0x40;
constexpr std::uint8_t trail_max = 0xFE;

constexpr std::uint16_t cp1252_high[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
    0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
};

void append_code_point(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
}

// Every single-byte code page maps one byte to one BMP code point; unmappable bytes decode to
// U+FFFD like Java's REPLACE action, which is what the fixed Jackcess does through
// Charset.decode().
void decode_single_byte(std::string& out, const std::uint16_t* table,
                        const std::vector<std::uint8_t>& bytes) {
    for (const std::uint8_t byte : bytes) {
        append_code_point(out, table[byte]);
    }
}

// Decodes a DBCS page by replaying the Java NIO results recorded in the generated table. A lead
// byte followed by a trail outside the table window replaces the lead and re-decodes the trail,
// mirroring the malformed-run behaviour verified by the generator. Both the GBK and Shift-JIS
// tables share this one-byte-or-two-byte layout, so the loop is templated on the page's tables.
template <std::size_t Rows>
void decode_dbcs(std::string& out, const std::vector<std::uint8_t>& bytes,
                 const std::uint16_t (&single)[256], const std::uint16_t (&double_rows)[Rows][191],
                 const std::uint8_t (&lead_row)[128]) {
    static_assert(Rows > 0, "DBCS tables need at least one lead row");
    static_assert(191 == (trail_max - trail_min + 1), "DBCS column count must match the trail window");
    std::size_t index = 0;
    while (index < bytes.size()) {
        const std::uint8_t byte = bytes[index];
        if (byte < 0x80) {
            append_code_point(out, single[byte]);
            ++index;
            continue;
        }
        const std::uint8_t row = lead_row[byte - 0x80];
        if (row == 255 || index + 1 >= bytes.size()) {
            // A non-lead high byte, or a lead at the end of the buffer, is a malformed run.
            append_code_point(out, single[byte]);
            ++index;
            continue;
        }
        const std::uint8_t trail = bytes[index + 1];
        if (trail > trail_max) {
            // 0xFF joins the malformed run: Java replaces the pair with a single U+FFFD.
            append_code_point(out, replacement);
            index += 2;
            continue;
        }
        if (trail < trail_min) {
            append_code_point(out, replacement);
            append_code_point(out, single[trail]);
            index += 2;
            continue;
        }
        const std::uint16_t value = double_rows[row][trail - trail_min];
        if (value == replacement) {
            append_code_point(out, replacement);
        } else if (value == replace_then_trail) {
            append_code_point(out, replacement);
            append_code_point(out, single[trail]);
        } else {
            append_code_point(out, value);
        }
        index += 2;
    }
}

}

std::string decode_code_page_text(const std::vector<std::uint8_t>& bytes, std::uint16_t code_page) {
    std::string result;
    result.reserve(bytes.size());
    switch (code_page) {
        case 1252: {
            for (const std::uint8_t byte : bytes) {
                std::uint32_t code_point = byte;
                if (byte >= 0x80 && byte <= 0x9f) {
                    code_point = cp1252_high[byte - 0x80];
                }
                append_code_point(result, code_point);
            }
            return result;
        }
        case 819:
        case 28591:
            // Latin-1 is the identity map.
            for (const std::uint8_t byte : bytes) {
                append_code_point(result, byte);
            }
            return result;
        case 1251:
            decode_single_byte(result, cp1251_single, bytes);
            return result;
        case 932:
            decode_dbcs(result, bytes, cp932_single, cp932_double, cp932_lead_row);
            return result;
        case 936:
            decode_dbcs(result, bytes, cp936_single, cp936_double, cp936_lead_row);
            return result;
        default:
            throw std::invalid_argument("Unsupported code page for Jet3 text: " + std::to_string(code_page));
    }
}

}
