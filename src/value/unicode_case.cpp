#include "unicode_case.hpp"

#include "text_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rea_jackcess::detail {
namespace {

struct CaseMapping {
    std::uint32_t code_point;
    std::uint32_t upper;
    std::uint32_t lower;
};

constexpr CaseMapping mappings[]{
#include "unicode_case_mappings.inc"
};

struct FullUppercaseMapping {
    std::uint32_t code_point;
    std::array<std::uint32_t, 3> output;
    std::uint8_t length;
};

constexpr FullUppercaseMapping full_uppercase_mappings[]{
#include "unicode_uppercase_mappings.inc"
};

const CaseMapping* find_mapping(std::uint32_t code_point) {
    const auto found = std::lower_bound(std::begin(mappings), std::end(mappings), code_point,
                                        [](const CaseMapping& mapping, std::uint32_t value) {
                                            return mapping.code_point < value;
                                        });
    return found != std::end(mappings) && found->code_point == code_point ? found : nullptr;
}

const FullUppercaseMapping* find_full_uppercase_mapping(std::uint32_t code_point) {
    const auto found = std::lower_bound(
        std::begin(full_uppercase_mappings), std::end(full_uppercase_mappings), code_point,
        [](const FullUppercaseMapping& mapping, std::uint32_t value) {
            return mapping.code_point < value;
        });
    return found != std::end(full_uppercase_mappings) && found->code_point == code_point
               ? found
               : nullptr;
}

std::uint32_t to_upper(std::uint32_t code_point) {
    const auto* mapping = find_mapping(code_point);
    return mapping ? mapping->upper : code_point;
}

std::uint32_t to_lower(std::uint32_t code_point) {
    const auto* mapping = find_mapping(code_point);
    return mapping ? mapping->lower : code_point;
}

std::uint32_t next_code_point(std::u16string_view text, std::size_t& offset) {
    const std::uint32_t first = text[offset++];
    if (first >= 0xd800 && first <= 0xdbff && offset < text.size()) {
        const std::uint32_t second = text[offset];
        if (second >= 0xdc00 && second <= 0xdfff) {
            ++offset;
            return 0x10000 + ((first - 0xd800) << 10) + (second - 0xdc00);
        }
    }
    return first;
}

void append_code_point(std::u16string& text, std::uint32_t code_point) {
    if (code_point <= 0xffff) {
        text.push_back(static_cast<char16_t>(code_point));
        return;
    }
    code_point -= 0x10000;
    text.push_back(static_cast<char16_t>(0xd800 + (code_point >> 10)));
    text.push_back(static_cast<char16_t>(0xdc00 + (code_point & 0x3ff)));
}

std::u16string uppercase_utf8(std::string_view text) {
    const std::u16string input = utf8_to_utf16(text, text.size(), "TEXT value is too long");
    std::u16string normalized;
    normalized.reserve(input.size());
    std::size_t offset = 0;
    while (offset < input.size()) {
        const auto code_point = next_code_point(input, offset);
        if (const auto* expansion = find_full_uppercase_mapping(code_point)) {
            for (std::size_t index = 0; index < expansion->length; ++index) {
                append_code_point(normalized, expansion->output[index]);
            }
        } else {
            append_code_point(normalized, to_upper(code_point));
        }
    }
    return normalized;
}

}

bool java_string_equals_ignore_case_utf8(std::string_view left, std::string_view right) {
    if (left == right) {
        return true;
    }
    if (left.size() == right.size()) {
        bool both_ascii = true;
        for (std::size_t index = 0; index < left.size(); ++index) {
            const auto left_byte = static_cast<std::uint8_t>(left[index]);
            const auto right_byte = static_cast<std::uint8_t>(right[index]);
            if (left_byte >= 0x80 || right_byte >= 0x80) {
                both_ascii = false;
                break;
            }
            const auto fold_ascii = [](std::uint8_t value) {
                return value >= 'A' && value <= 'Z'
                           ? static_cast<std::uint8_t>(value + ('a' - 'A'))
                           : value;
            };
            if (fold_ascii(left_byte) != fold_ascii(right_byte)) {
                return false;
            }
        }
        if (both_ascii) {
            return true;
        }
    }

    std::u16string left_utf16;
    std::u16string right_utf16;
    try {
        left_utf16 = utf8_to_utf16(left, left.size(), "TEXT value is too long");
        right_utf16 = utf8_to_utf16(right, right.size(), "TEXT value is too long");
    } catch (const std::invalid_argument&) {
        return false;
    }
    if (left_utf16.size() != right_utf16.size()) {
        return false;
    }

    std::size_t left_offset = 0;
    std::size_t right_offset = 0;
    while (left_offset < left_utf16.size()) {
        const std::uint32_t left_code_point = next_code_point(left_utf16, left_offset);
        const std::uint32_t right_code_point = next_code_point(right_utf16, right_offset);
        if (left_code_point == right_code_point) {
            continue;
        }

        const std::uint32_t left_upper = to_upper(left_code_point);
        const std::uint32_t right_upper = to_upper(right_code_point);
        if (left_upper != right_upper && to_lower(left_upper) != to_lower(right_upper)) {
            return false;
        }
    }
    return true;
}

bool java_string_uppercase_key_equal_utf8(std::string_view left, std::string_view right) {
    if (left == right) {
        return true;
    }
    try {
        return uppercase_utf8(left) == uppercase_utf8(right);
    } catch (const std::invalid_argument&) {
        return false;
    }
}

}
