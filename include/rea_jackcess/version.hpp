#pragma once

#include <string_view>

namespace rea_jackcess {

// Returns the library version string from CMake's PROJECT_VERSION (e.g. "0.0.0"). The returned
// view points at static storage and stays valid for the program's lifetime.
std::string_view version() noexcept;

}
