#pragma once

#include "rea_jackcess/database_file.hpp"
#include "rea_jackcess/table.hpp"

#include <filesystem>

namespace rea_jackcess::detail {

// Writes a new empty table described by `table` into the Jet4 database file at `path`: table
// definition pages, empty index roots, usage maps and the MSysObjects catalog entry. Called by
// Database::create once per table against a staged database file. Only V2000/V2003 are supported.
void create_jet4_table(const std::filesystem::path& path,
                       DatabaseFormat format,
                       const TableBuilder& table);

}
