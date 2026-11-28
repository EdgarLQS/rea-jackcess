#include "jet_format.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace rea_jackcess {

const char* FormatInfo::name() const noexcept {
    switch (version) {
        case EngineVersion::jet3:
            return "Jet3";
        case EngineVersion::jet4:
            return "Jet4";
        case EngineVersion::ace2007:
            return "ACE2007";
        case EngineVersion::ace2010:
            return "ACE2010";
        case EngineVersion::ace2016:
            return "ACE2016";
        case EngineVersion::ace2019:
            return "ACE2019";
        case EngineVersion::msisam:
            return "MSISAM";
    }
    return "Unknown";
}

}

namespace rea_jackcess::detail {
namespace {

constexpr std::size_t engine_name_offset = 4;
constexpr std::uint64_t version_offset = 20;
constexpr std::size_t code_page_offset = 60;
constexpr std::size_t encoding_key_size = 4;

// Jackcess BASE_HEADER_MASK, truncated at the last byte the read-only path needs (the encoding
// key ends at offset 65). Masks are indexed from header_mask_offset.
constexpr std::array<std::uint8_t, 42> header_mask{
    0xB5, 0x6F, 0x03, 0x62, 0x61, 0x08, 0xC2, 0x55, 0xEB, 0xA9, 0x67, 0x72, 0x43, 0x3F,
    0x00, 0x9C, 0x7A, 0x9F, 0x90, 0xFF, 0x80, 0x9A, 0x31, 0xC5, 0x79, 0xBA, 0xED, 0x30,
    0xBC, 0xDF, 0xCC, 0x9D, 0x63, 0xD9, 0xE4, 0xC3, 0x7B, 0x42, 0xFB, 0x8A, 0xBC, 0x4E};
constexpr std::array<char, 15> msisam_engine_name{
    'M', 'S', 'I', 'S', 'A', 'M', ' ', 'D', 'a', 't', 'a', 'b', 'a', 's', 'e'};

bool is_msisam(const std::array<std::uint8_t, database_header_size>& header) {
    for (std::size_t index = 0; index < msisam_engine_name.size(); ++index) {
        if (header[engine_name_offset + index] != static_cast<std::uint8_t>(msisam_engine_name[index])) {
            return false;
        }
    }
    return true;
}

}

std::uint8_t header_mask_byte(std::size_t offset) noexcept {
    if (offset < header_mask_offset) {
        return 0;
    }
    const std::size_t index = offset - header_mask_offset;
    return index < header_mask.size() ? header_mask[index] : 0;
}

bool has_jet4_page_layout(EngineVersion version) noexcept {
    switch (version) {
        case EngineVersion::jet4:
        case EngineVersion::ace2007:
        case EngineVersion::ace2010:
        case EngineVersion::ace2016:
        case EngineVersion::ace2019:
            return true;
        case EngineVersion::jet3:
        case EngineVersion::msisam:
            return false;
    }
    return false;
}

FormatInfo detect_format(const std::array<std::uint8_t, database_header_size>& header,
                         const std::filesystem::path& path) {
    const std::uint8_t version = header[version_offset];
    switch (version) {
        case 0:
            return {EngineVersion::jet3, 2048, true};
        case 1:
            return {is_msisam(header) ? EngineVersion::msisam : EngineVersion::jet4, 4096, false};
        case 2:
            return {EngineVersion::ace2007, 4096, false};
        case 3:
            return {EngineVersion::ace2010, 4096, false};
        case 5:
            return {EngineVersion::ace2016, 4096, false};
        case 6:
            return {EngineVersion::ace2019, 4096, false};
        default:
            throw DatabaseError(DatabaseErrorCode::unsupported_version,
                                "Unsupported database engine version " + std::to_string(version), path);
    }
}

std::optional<std::uint16_t> decode_jet4_code_page(const std::vector<std::uint8_t>& raw_root_page) {
    if (raw_root_page.size() < code_page_offset + 2) {
        return std::nullopt;
    }
    // These are BASE_HEADER_MASK bytes 36-37 at root-page offset 24, per Jackcess 5.2.0.
    const std::uint8_t low = raw_root_page[code_page_offset] ^ header_mask_byte(code_page_offset);
    const std::uint8_t high = raw_root_page[code_page_offset + 1] ^ header_mask_byte(code_page_offset + 1);
    return static_cast<std::uint16_t>(low) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(high) << 8);
}

bool root_page_declares_encoding(const std::vector<std::uint8_t>& raw_root_page) noexcept {
    if (raw_root_page.size() < encoding_key_offset + encoding_key_size) {
        return false;
    }
    // DefaultCodecProvider reads the key as a 4-byte little-endian int; any non-zero value means
    // the pages are encoded.
    std::uint32_t key = 0;
    for (std::size_t index = 0; index < encoding_key_size; ++index) {
        const std::size_t offset = encoding_key_offset + index;
        const std::uint8_t value = raw_root_page[offset] ^ header_mask_byte(offset);
        key |= static_cast<std::uint32_t>(value) << (8 * index);
    }
    return key != 0;
}

bool root_page_declares_ace_encryption(const std::vector<std::uint8_t>& raw_root_page) noexcept {
    // MS-OFFCRYPTO EncryptionInfo: u16 length at 0x299, payload at 0x29B. Both bytes lie outside
    // the BASE_HEADER_MASK range, so they are read verbatim.
    constexpr std::size_t length_offset = 0x299;
    if (raw_root_page.size() < length_offset + 2) {
        return false;
    }
    return raw_root_page[length_offset] != 0 || raw_root_page[length_offset + 1] != 0;
}


}
