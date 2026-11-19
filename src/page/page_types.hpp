#pragma once

#include <cstdint>

namespace rea_jackcess::detail {

enum class PageType : std::uint8_t {
    invalid = 0x00,
    data = 0x01,
    table_definition = 0x02,
    index_node = 0x03,
    index_leaf = 0x04,
    usage_map = 0x05,
};

}
