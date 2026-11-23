#include "jet4_text_key.hpp"

#include <charconv>
#include <optional>
#include <stdexcept>

namespace rea_jackcess::detail {
namespace {

// Character-weight tables from the fixed Jackcess 5.2.0 release (Apache-2.0).
#include "jet4_text_index_weights.inc"

using Bytes = std::vector<std::uint8_t>;

struct CharacterWeights {
    Bytes primary;
    Bytes secondary;
    Bytes unprintable;
    std::uint8_t secondary_modifier = 0;
    std::uint8_t kana = 0;
    bool simple = false;
    bool significant = false;
};

Bytes parse_hex(std::string_view text) {
    Bytes bytes;
    while (!text.empty()) {
        const auto width = 2 - text.size() % 2;
        unsigned value = 0;
        const auto result = std::from_chars(text.data(), text.data() + width, value, 16);
        if (result.ec != std::errc{} || result.ptr != text.data() + width) {
            throw std::logic_error("Invalid Jet4 text-index weight");
        }
        bytes.push_back(static_cast<std::uint8_t>(value));
        text.remove_prefix(width);
    }
    return bytes;
}

CharacterWeights parse_weights(std::string_view line) {
    if (line.empty()) {
        throw std::logic_error("Empty Jet4 text-index weight");
    }
    const char kind = line.front();
    line.remove_prefix(1);
    const auto first_comma = line.find(',');
    CharacterWeights weights;
    switch (kind) {
        case 'S':
            weights.primary = parse_hex(line);
            weights.simple = true;
            break;
        case 'I':
            if (first_comma == std::string_view::npos) {
                throw std::logic_error("Invalid international Jet4 text-index weight");
            }
            weights.primary = parse_hex(line.substr(0, first_comma));
            weights.secondary = parse_hex(line.substr(first_comma + 1));
            break;
        case 'U':
            weights.unprintable = parse_hex(line);
            break;
        case 'P':
            weights.secondary_modifier = parse_hex(line).at(0);
            break;
        case 'Z': {
            const auto last_comma = line.rfind(',');
            if (first_comma == std::string_view::npos || last_comma <= first_comma) {
                throw std::logic_error("Invalid extended international Jet4 text-index weight");
            }
            weights.primary = parse_hex(line.substr(0, first_comma));
            weights.secondary = parse_hex(line.substr(first_comma + 1, last_comma - first_comma - 1));
            weights.kana = line.substr(last_comma + 1) == "1" ? 2 : 3;
            break;
        }
        case 'G':
            weights.primary = parse_hex(line);
            weights.significant = true;
            break;
        case 'X':
            break;
        default:
            throw std::logic_error("Unknown Jet4 text-index weight type");
    }
    return weights;
}

void load_weights(std::vector<CharacterWeights>& weights,
                  std::size_t start,
                  std::size_t end,
                  std::string_view lines) {
    for (std::size_t unit = start; unit < end; ++unit) {
        if (unit >= 0xd800 && unit <= 0xdfff) {
            continue;
        }
        const auto newline = lines.find('\n');
        if (lines.empty()) {
            throw std::logic_error("Incomplete Jet4 text-index weight table");
        }
        weights[unit] = parse_weights(lines.substr(0, newline));
        lines.remove_prefix(newline == std::string_view::npos ? lines.size() : newline + 1);
    }
    if (!lines.empty()) {
        throw std::logic_error("Extra Jet4 text-index weights");
    }
}

void set_surrogate_weight(CharacterWeights& weight, std::uint16_t inline_value, std::uint8_t extra) {
    weight.primary = {static_cast<std::uint8_t>(inline_value >> 8),
                      static_cast<std::uint8_t>(inline_value)};
    weight.secondary = {extra};
}

std::uint16_t surrogate_inline_value(std::uint16_t code_unit, std::int32_t offset) {
    return static_cast<std::uint16_t>(static_cast<std::int32_t>(code_unit) + offset);
}

const std::vector<CharacterWeights>& general_legacy_weights() {
    static const auto weights = [] {
        std::vector<CharacterWeights> table(65536);
        load_weights(table, 0, 256, {basic_weights, sizeof(basic_weights) - 1});
        load_weights(table, 256, table.size(), {extended_weights, sizeof(extended_weights) - 1});
        return table;
    }();
    return weights;
}

const std::vector<CharacterWeights>& general_weights() {
    static const auto weights = [] {
        std::vector<CharacterWeights> table(65536);
        load_weights(table, 0, 256, {general_basic_weights, sizeof(general_basic_weights) - 1});
        load_weights(table, 256, table.size(), {general_extended_weights, sizeof(general_extended_weights) - 1});

        for (std::uint16_t unit = 0xd800; unit <= 0xd83f; ++unit) {
            set_surrogate_weight(table[unit], surrogate_inline_value(unit, -10238), 0x3f);
        }
        for (std::uint16_t unit = 0xd840; unit <= 0xd87f; ++unit) {
            set_surrogate_weight(table[unit], surrogate_inline_value(unit, 9666), 0x3e);
        }
        for (std::uint16_t unit = 0xdb80; unit <= 0xdbff; ++unit) {
            set_surrogate_weight(table[unit], surrogate_inline_value(unit, 9090), 0x3e);
        }
        for (std::uint16_t unit = 0xdc00; unit <= 0xdfff; ++unit) {
            const auto offset = static_cast<std::uint16_t>(unit - 0xdc00);
            const auto index_offset = offset < 8 ? 9992 :
                offset < 8 + 254 ? 9990 :
                offset < 8 + 254 + 254 ? 9988 :
                offset < 8 + 254 + 254 + 254 ? 9986 : 9984;
            set_surrogate_weight(table[unit], static_cast<std::uint16_t>(unit - index_offset), 0x3f);
        }
        return table;
    }();
    return weights;
}

const std::vector<CharacterWeights>& general97_weights() {
    static const auto weights = [] {
        std::vector<CharacterWeights> table(256);
        load_weights(table, 0, table.size(), {general97_basic_weights, sizeof(general97_basic_weights) - 1});
        return table;
    }();
    return weights;
}

constexpr std::uint16_t general97_first_mapped_char = 338;
constexpr std::uint16_t general97_last_mapped_char = 8482;

const std::vector<std::uint16_t>& general97_mappings() {
    static const auto mappings = [] {
        std::vector<std::uint16_t> table(general97_last_mapped_char - general97_first_mapped_char + 1);
        std::string_view lines{general97_mapping_data, sizeof(general97_mapping_data) - 1};
        while (!lines.empty()) {
            const auto newline = lines.find('\n');
            auto line = lines.substr(0, newline);
            const auto comma = line.find(',');
            if (comma == std::string_view::npos) {
                throw std::logic_error("Invalid General 97 extended text-index mapping");
            }
            unsigned source = 0;
            unsigned target = 0;
            const auto source_result = std::from_chars(line.data(), line.data() + comma, source);
            const auto target_result = std::from_chars(line.data() + comma + 1, line.data() + line.size(), target);
            if (source_result.ec != std::errc{} || source_result.ptr != line.data() + comma ||
                target_result.ec != std::errc{} || target_result.ptr != line.data() + line.size() ||
                source < general97_first_mapped_char || source > general97_last_mapped_char || target > 0xff) {
                throw std::logic_error("Invalid General 97 extended text-index mapping");
            }
            table[source - general97_first_mapped_char] = static_cast<std::uint16_t>(target);
            lines.remove_prefix(newline == std::string_view::npos ? lines.size() : newline + 1);
        }
        return table;
    }();
    return mappings;
}

const CharacterWeights& general97_weight(std::uint16_t code_unit) {
    const auto& weights = general97_weights();
    if (code_unit < weights.size()) {
        return weights[code_unit];
    }
    if (code_unit < general97_first_mapped_char || code_unit > general97_last_mapped_char) {
        static const CharacterWeights ignored;
        return ignored;
    }
    const auto mapped_unit = general97_mappings()[code_unit - general97_first_mapped_char];
    return weights[mapped_unit];
}

void append(Bytes& output, const Bytes& bytes) {
    output.insert(output.end(), bytes.begin(), bytes.end());
}

void append_kana(Bytes& key, Bytes kana) {
    const auto repeat_count = (kana.size() + 6) / 7;
    while (!kana.empty() && kana.back() == 3) {
        kana.pop_back();
    }
    for (std::size_t index = 0; index < kana.size(); index += 3) {
        std::uint8_t packed = 0x80;
        for (std::size_t part = 0; part < 3 && index + part < kana.size(); ++part) {
            packed |= static_cast<std::uint8_t>(kana[index + part] << (4 - part * 2));
        }
        key.push_back(packed);
    }
    key.insert(key.end(), {0xff, 0x02});
    key.insert(key.end(), repeat_count, 0x80);
    key.push_back(0xff);
    key.insert(key.end(), repeat_count, 0x80);
}

Bytes encode_general_weight_key(std::u16string_view text,
                                bool descending,
                                const std::vector<CharacterWeights>& weights) {
    constexpr std::uint8_t secondary_placeholder = 0x02;
    text = text.substr(0, 255);
    while (!text.empty() && text.back() == u' ') {
        text.remove_suffix(1);
    }

    Bytes key{static_cast<std::uint8_t>(descending ? 0x80 : 0x7f)};
    Bytes secondary;
    Bytes unprintable;
    Bytes kana;
    std::size_t primary_count = 0;
    std::size_t secondary_count = 0;
    std::size_t secondary_prefix_length = 0;
    for (char16_t unit : text) {
        const auto position = primary_count;
        const auto& weight = weights[unit];
        if (!weight.primary.empty()) {
            append(key, weight.primary);
            ++primary_count;
        }
        if (!weight.secondary.empty() || weight.secondary_modifier != 0) {
            if (secondary_count < position) {
                secondary.insert(secondary.end(), position - secondary_count, secondary_placeholder);
                secondary_count = position;
            }
            if (!weight.secondary.empty()) {
                append(secondary, weight.secondary);
                ++secondary_count;
            } else if (!secondary.empty()) {
                secondary.back() += weight.secondary_modifier;
            } else {
                secondary.push_back(weight.secondary_modifier);
                secondary_prefix_length = 1;
            }
        }
        if (!weight.unprintable.empty()) {
            const auto offset = (7 + 4 * (position + secondary.size() -
                                          secondary_count - secondary_prefix_length)) | 0x8000;
            unprintable.push_back(static_cast<std::uint8_t>(offset >> 8));
            unprintable.push_back(static_cast<std::uint8_t>(offset));
            unprintable.push_back(0x06);
            append(unprintable, weight.unprintable);
        }
        if (weight.kana != 0) {
            kana.push_back(weight.kana);
        }
    }

    key.push_back(0x01);
    while (!secondary.empty() && secondary.back() <= secondary_placeholder) {
        secondary.pop_back();
    }
    append(key, secondary);
    if (!kana.empty() || !unprintable.empty()) {
        key.insert(key.end(), {0x01, 0x01});
        if (!kana.empty()) {
            append_kana(key, kana);
            if (!unprintable.empty()) {
                key.push_back(0xff);
            }
        }
        if (!unprintable.empty()) {
            key.push_back(0x01);
            append(key, unprintable);
        }
    }
    if (descending) {
        key.push_back(0);
        for (auto byte = key.begin() + 1; byte != key.end(); ++byte) {
            *byte ^= 0xff;
        }
    }
    key.push_back(0);
    return key;
}

class NibbleStream {
public:
    void write_nibble(std::uint8_t value) {
        value &= 0x0f;
        if (nibble_length_ % 2 == 0) {
            bytes_.push_back(static_cast<std::uint8_t>(value << 4));
        } else {
            bytes_.back() |= value;
        }
        ++nibble_length_;
    }

    void write_fill_nibbles(std::size_t count, std::uint8_t value) {
        for (std::size_t i = 0; i < count; ++i) {
            write_nibble(value);
        }
    }

    const Bytes& bytes() const noexcept {
        return bytes_;
    }

private:
    Bytes bytes_;
    std::size_t nibble_length_ = 0;
};

Bytes encode_general97_weight_key(std::u16string_view text, bool descending) {
    text = text.substr(0, 255);
    while (!text.empty() && text.back() == u' ') {
        text.remove_suffix(1);
    }

    Bytes key{static_cast<std::uint8_t>(descending ? 0x80 : 0x7f)};
    const auto payload_start = key.size();
    std::optional<NibbleStream> extra_codes;
    std::size_t significant_char_count = 0;
    for (const char16_t unit : text) {
        const auto& weight = general97_weight(static_cast<std::uint16_t>(unit));
        append(key, weight.primary);
        if (weight.simple) {
            continue;
        }
        if (weight.significant) {
            ++significant_char_count;
            continue;
        }
        if (!weight.secondary.empty()) {
            if (weight.secondary.size() != 1) {
                throw std::logic_error("Unexpected General 97 secondary text-index weight length");
            }
            if (!extra_codes) {
                extra_codes.emplace();
                extra_codes->write_nibble(0);
            }
            extra_codes->write_fill_nibbles(significant_char_count, 0x02);
            extra_codes->write_nibble(weight.secondary.front());
            significant_char_count = 0;
        }
    }
    if (extra_codes) {
        extra_codes->write_nibble(0);
        append(key, extra_codes->bytes());
    } else {
        key.push_back(0);
    }
    if (descending) {
        for (auto byte = key.begin() + static_cast<std::ptrdiff_t>(payload_start); byte != key.end(); ++byte) {
            *byte ^= 0xff;
        }
    }
    return key;
}

}

std::vector<std::uint8_t> encode_jet4_general_legacy_key(std::u16string_view text,
                                                        bool descending) {
    return encode_general_weight_key(text, descending, general_legacy_weights());
}

std::vector<std::uint8_t> encode_jet4_general_key(std::u16string_view text,
                                                  bool descending) {
    return encode_general_weight_key(text, descending, general_weights());
}

std::vector<std::uint8_t> encode_jet4_general97_key(std::u16string_view text,
                                                    bool descending) {
    return encode_general97_weight_key(text, descending);
}

}
