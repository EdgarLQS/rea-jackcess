#pragma once

#include "rea_jackcess/table.hpp"

#include <array>
#include <cstdint>

namespace rea_jackcess::detail {

inline constexpr std::uint8_t jet4_binary_type_code = 0x09;
inline constexpr std::uint8_t jet4_text_type_code = 0x0a;

struct Jet4VariableType {
    DataType public_type;
    std::uint8_t jet_type_code;
    std::uint16_t maximum_byte_size;
};

inline constexpr std::array<Jet4VariableType, 2> jet4_variable_types{{
    {DataType::binary, jet4_binary_type_code, 255},
    {DataType::text, jet4_text_type_code, 510},
}};

inline const Jet4VariableType* jet4_variable_type(DataType type) noexcept {
    for (const auto& candidate : jet4_variable_types) {
        if (candidate.public_type == type) {
            return &candidate;
        }
    }
    return nullptr;
}

inline const Jet4VariableType* jet4_variable_type(std::uint8_t type_code) noexcept {
    for (const auto& candidate : jet4_variable_types) {
        if (candidate.jet_type_code == type_code) {
            return &candidate;
        }
    }
    return nullptr;
}

}
