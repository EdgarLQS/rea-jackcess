#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

// Returns the SHA-256 of `bytes` as a lowercase hex string.
std::string sha256_hex(const std::vector<std::uint8_t>& bytes);
// Returns the SHA-256 of the file at `path` as a lowercase hex string.
// Throws DatabaseError (io_error) when the file cannot be read.
std::string sha256_file_hex(const std::filesystem::path& path);

}
