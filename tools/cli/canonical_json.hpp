#pragma once

#include "page/page_file.hpp"

#include <filesystem>
#include <iosfwd>

namespace rea_jackcess::detail {

void write_canonical_json(std::ostream& output,
                          const PageFile& file,
                          const std::filesystem::path& source_path);

}
