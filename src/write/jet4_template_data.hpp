#pragma once

#include "rea_jackcess/database_file.hpp"

#include <cstddef>
#include <cstdint>

namespace rea_jackcess::detail {

// A read-only view of an embedded empty-database template.
struct Jet4TemplateView {
    const std::uint8_t* data; // Template bytes (statically owned).
    std::size_t size;         // Template length in bytes.
};

// Returns the embedded empty-database template for `format` (v2000 or v2003), used to seed a new file.
Jet4TemplateView jet4_template(DatabaseFormat format) noexcept;

}
