#pragma once

#include "../page/page_file.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace rea_jackcess::detail {

struct IndexRowLocation {
    std::uint32_t page_number;
    std::uint16_t row_number;
};

class IndexEntryCursor {
public:
    IndexEntryCursor(const PageFile& file,
                     std::string table_name,
                     std::uint64_t table_definition_page_number,
                     std::uint32_t root_page_number);
    ~IndexEntryCursor();

    IndexEntryCursor(IndexEntryCursor&& other) noexcept;
    IndexEntryCursor& operator=(IndexEntryCursor&& other) noexcept;

    IndexEntryCursor(const IndexEntryCursor&) = delete;
    IndexEntryCursor& operator=(const IndexEntryCursor&) = delete;

    std::optional<IndexRowLocation> next();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
