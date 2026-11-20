#pragma once

#include "rea_jackcess/database_file.hpp"

#include <cstddef>
#include <cstdint>

namespace rea_jackcess::detail {

struct Jet4TemplateView {
    const std::uint8_t* data;
    std::size_t size;
};

Jet4TemplateView jet4_template(DatabaseFormat format) noexcept;

}
