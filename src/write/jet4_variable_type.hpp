#pragma once

#include "rea_jackcess/table.hpp"

#include <array>
#include <cstdint>

namespace rea_jackcess::detail {

inline constexpr std::uint8_t jet4_binary_type_code = 0x09;
inline constexpr std::uint8_t jet4_text_type_code = 0x0a;
inline constexpr std::uint8_t jet4_ole_type_code = 0x0b;
inline constexpr std::uint8_t jet4_memo_type_code = 0x0c;

struct Jet4VariableType {
    DataType public_type;
    std::uint8_t jet_type_code;
    std::uint16_t maximum_byte_size;
};

inline constexpr std::array<Jet4VariableType, 4> jet4_variable_types{{
    {DataType::binary, jet4_binary_type_code, 255},
    {DataType::text, jet4_text_type_code, 510},
    {DataType::ole, jet4_ole_type_code, 0},
    {DataType::memo, jet4_memo_type_code, 0},
}};

inline bool jet4_is_long_value_type(DataType type) noexcept {
    return type == DataType::ole || type == DataType::memo;
}

inline bool jet4_is_long_value_type(std::uint8_t type_code) noexcept {
    return type_code == jet4_ole_type_code || type_code == jet4_memo_type_code;
}

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
