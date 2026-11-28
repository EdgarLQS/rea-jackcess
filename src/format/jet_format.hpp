#pragma once

#include "rea_jackcess/database_file.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace rea_jackcess::detail {

// Bytes of the database header examined to choose the engine.
constexpr std::size_t database_header_size = 21;

// Jackcess masks the root page from offset 24 with a fixed pattern, so the code page and the
// encoding key are only visible after unmasking.
constexpr std::size_t header_mask_offset = 24;  // Absolute root-page offset where masking starts.
constexpr std::size_t encoding_key_offset = 62;  // Absolute root-page offset of the encoding key.

// Detects the engine version from the header bytes. `path` is used only for error context.
FormatInfo detect_format(const std::array<std::uint8_t, database_header_size>& header,
                         const std::filesystem::path& path);
// ACE versions retain the Jet4 page, table-definition, usage-map and row layouts for reads.
bool has_jet4_page_layout(EngineVersion version) noexcept;
// Decodes the Jet4 code page from the (unmasked) root page bytes; nullopt when absent/invalid.
std::optional<std::uint16_t> decode_jet4_code_page(const std::vector<std::uint8_t>& raw_root_page);
// The fixed mask byte stored at an absolute root-page offset; zero outside the masked range.
std::uint8_t header_mask_byte(std::size_t offset) noexcept;
// True when the root page carries a non-zero encoding key, i.e. the pages need a codec.
bool root_page_declares_encoding(const std::vector<std::uint8_t>& raw_root_page) noexcept;

}
