#pragma once

#include "rea_jackcess/table.hpp"

#include <array>
#include <cstdint>

namespace rea_jackcess::detail {

// BOOLEAN values live in the row NULL mask, not in the fixed data section: the
// column definition carries length 1 and a fixed data offset of 0, and the column
// contributes no bytes to the fixed data area.
inline constexpr std::uint8_t jet4_boolean_type_code = 0x01;
inline constexpr std::uint8_t jet4_byte_type_code = 0x02;
inline constexpr std::uint8_t jet4_short_type_code = 0x03;
inline constexpr std::uint8_t jet4_long_type_code = 0x04;
inline constexpr std::uint8_t jet4_money_type_code = 0x05;
inline constexpr std::uint8_t jet4_float_type_code = 0x06;
inline constexpr std::uint8_t jet4_double_type_code = 0x07;
inline constexpr std::uint8_t jet4_guid_type_code = 0x0f;

struct Jet4FixedType {
    DataType public_type;
    std::uint8_t jet_type_code;
    std::uint16_t byte_size;
};

inline constexpr std::array<Jet4FixedType, 7> jet4_fixed_types{{
    {DataType::byte, jet4_byte_type_code, 1},
    {DataType::int16, jet4_short_type_code, 2},
    {DataType::int32, jet4_long_type_code, 4},
    {DataType::money, jet4_money_type_code, 8},
    {DataType::float32, jet4_float_type_code, 4},
    {DataType::float64, jet4_double_type_code, 8},
    {DataType::guid, jet4_guid_type_code, 16},
}};

inline bool jet4_is_boolean_type(DataType type) noexcept {
    return type == DataType::boolean;
}

inline bool jet4_is_boolean_type(std::uint8_t type_code) noexcept {
    return type_code == jet4_boolean_type_code;
}

inline const Jet4FixedType* jet4_fixed_type(DataType type) noexcept {
    for (const auto& candidate : jet4_fixed_types) {
        if (candidate.public_type == type) {
            return &candidate;
        }
    }
    return nullptr;
}

inline const Jet4FixedType* jet4_fixed_type(std::uint8_t type_code) noexcept {
    for (const auto& candidate : jet4_fixed_types) {
        if (candidate.jet_type_code == type_code) {
            return &candidate;
        }
    }
    return nullptr;
}

}
