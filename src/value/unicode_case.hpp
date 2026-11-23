#pragma once

#include <string_view>

namespace rea_jackcess::detail {

// Compares valid UTF-8 using Java String.equalsIgnoreCase's locale-independent simple mappings.
bool java_string_equals_ignore_case_utf8(std::string_view left, std::string_view right);

// Compares the keys produced by String.toUpperCase(Locale.ROOT), including full expansions.
bool java_string_uppercase_key_equal_utf8(std::string_view left, std::string_view right);

}
