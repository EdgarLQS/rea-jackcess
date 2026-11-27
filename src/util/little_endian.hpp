#pragma once

#include <cstdint>
#include <vector>

// Little-endian fixed-width integer helpers shared by the read and write paths. `offset` is a byte
// index into `bytes`; the caller must ensure the access is in range.
namespace rea_jackcess::detail {

// Reads a 2-byte little-endian unsigned integer at `offset`.
inline std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

// Reads a 4-byte little-endian unsigned integer at `offset`.
inline std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

// Reads an 8-byte little-endian unsigned integer at `offset`.
inline std::uint64_t read_u64(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    return static_cast<std::uint64_t>(read_u32(bytes, offset)) |
           (static_cast<std::uint64_t>(read_u32(bytes, offset + 4)) << 32);
}

// Writes `value` as a 2-byte little-endian integer at `offset` (in place).
inline void write_u16(std::vector<std::uint8_t>& bytes, std::uint64_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
}

// Writes `value` as a 4-byte little-endian integer at `offset` (in place).
inline void write_u32(std::vector<std::uint8_t>& bytes, std::uint64_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xff);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
}

// Writes `value` as an 8-byte little-endian integer at `offset` (in place).
inline void write_u64(std::vector<std::uint8_t>& bytes, std::uint64_t offset, std::uint64_t value) {
    write_u32(bytes, offset, static_cast<std::uint32_t>(value));
    write_u32(bytes, offset + 4, static_cast<std::uint32_t>(value >> 32));
}

}
