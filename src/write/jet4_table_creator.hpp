#pragma once

#include "rea_jackcess/database_file.hpp"
#include "rea_jackcess/table.hpp"

#include <filesystem>

namespace rea_jackcess::detail {

void create_jet4_table(const std::filesystem::path& path,
                       DatabaseFormat format,
                       const TableBuilder& table);

}
