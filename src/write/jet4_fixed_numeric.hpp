#pragma once

#include "rea_jackcess/table.hpp"

#include <array>
#include <cstdint>

namespace rea_jackcess::detail {

inline constexpr std::uint8_t jet4_byte_type_code = 0x02;
inline constexpr std::uint8_t jet4_short_type_code = 0x03;
inline constexpr std::uint8_t jet4_long_type_code = 0x04;
inline constexpr std::uint8_t jet4_float_type_code = 0x06;
inline constexpr std::uint8_t jet4_double_type_code = 0x07;

struct Jet4FixedNumericType {
    DataType public_type;
    std::uint8_t jet_type_code;
    std::uint16_t byte_size;
};

inline constexpr std::array<Jet4FixedNumericType, 5> jet4_fixed_numeric_types{{
    {DataType::byte, jet4_byte_type_code, 1},
    {DataType::int16, jet4_short_type_code, 2},
    {DataType::int32, jet4_long_type_code, 4},
    {DataType::float32, jet4_float_type_code, 4},
    {DataType::float64, jet4_double_type_code, 8},
}};

inline const Jet4FixedNumericType* jet4_fixed_numeric_type(DataType type) noexcept {
    for (const auto& candidate : jet4_fixed_numeric_types) {
        if (candidate.public_type == type) {
            return &candidate;
        }
    }
    return nullptr;
}

inline const Jet4FixedNumericType* jet4_fixed_numeric_type(std::uint8_t type_code) noexcept {
    for (const auto& candidate : jet4_fixed_numeric_types) {
        if (candidate.jet_type_code == type_code) {
            return &candidate;
        }
    }
    return nullptr;
}

}
