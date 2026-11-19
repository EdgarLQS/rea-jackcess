#include "text_codec.hpp"

namespace rea_jackcess::detail {
namespace {

void append_utf8(std::string& output, std::uint32_t code_point) {
    if (code_point <= 0x7f) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else if (code_point <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

void append_utf16le_segment(std::string& output,
                            const std::vector<std::uint8_t>& bytes,
                            std::size_t begin,
                            std::size_t end) {
    std::size_t offset = begin;
    while (offset + 1 < end) {
        std::uint32_t code_point = read_u16(bytes, offset);
        offset += 2;
        if (code_point >= 0xd800 && code_point <= 0xdbff) {
            if (offset + 1 < end) {
                const std::uint32_t low = read_u16(bytes, offset);
                if (low >= 0xdc00 && low <= 0xdfff) {
                    code_point = 0x10000 + ((code_point - 0xd800) << 10) + (low - 0xdc00);
                    offset += 2;
                } else {
                    code_point = 0xfffd;
                }
            } else {
                code_point = 0xfffd;
            }
        } else if (code_point >= 0xdc00 && code_point <= 0xdfff) {
            code_point = 0xfffd;
        }
        append_utf8(output, code_point);
    }
    if (offset < end) {
        append_utf8(output, 0xfffd);
    }
}

}

std::string decode_utf16le(const std::vector<std::uint8_t>& bytes) {
    std::string result;
    result.reserve(bytes.size());
    append_utf16le_segment(result, bytes, 0, bytes.size());
    return result;
}

std::string decode_jet4_text(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 2 || bytes[0] != 0xff || bytes[1] != 0xfe) {
        return decode_utf16le(bytes);
    }

    std::string result;
    result.reserve(bytes.size());
    std::size_t segment_start = 2;
    bool compressed = true;
    for (std::size_t offset = 2; offset < bytes.size(); ++offset) {
        if (bytes[offset] != 0) {
            continue;
        }
        if (compressed) {
            for (std::size_t index = segment_start; index < offset; ++index) {
                append_utf8(result, bytes[index]);
            }
        } else {
            append_utf16le_segment(result, bytes, segment_start, offset);
        }
        compressed = !compressed;
        segment_start = offset + 1;
    }

    if (compressed) {
        for (std::size_t index = segment_start; index < bytes.size(); ++index) {
            append_utf8(result, bytes[index]);
        }
    } else {
        append_utf16le_segment(result, bytes, segment_start, bytes.size());
    }
    return result;
}

}
