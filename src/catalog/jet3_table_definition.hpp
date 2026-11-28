#pragma once

#include "table_definition.hpp"

#include <cstdint>

namespace rea_jackcess::detail {

// Reads a Jet3 (V1997) table definition page. Column definitions are supported; index
// definitions and row data are handled by later stages, so both index lists stay empty.
// `page_number` is the page holding the table definition.
TableDefinition read_jet3_table_definition(const PageFile& file, std::uint64_t page_number);

}  // namespace rea_jackcess::detail
