#pragma once

#include "../page/page_file.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

struct IndexRowLocation {
    std::uint32_t page_number;
    std::uint16_t row_number;
};

struct IndexEntryRecord {
    IndexRowLocation row;
    // Raw ascending entry key bytes, so callers can compare against an encoded search key.
    std::vector<std::uint8_t> key;
};

class IndexEntryCursor {
public:
    // Scans every entry when no bounds are given. start_key positions the cursor at the first
    // entry whose key is not less than it; stop_key makes the cursor stop at the first entry
    // whose key is not less than it (exclusive end). Keys compare in encoded byte order, which
    // is the order the index is sorted by.
    // `reverse` walks the same leaf chain from right to left over the same [start, stop)
    // interval, starting below the exclusive upper bound and ending at the inclusive lower bound.
    IndexEntryCursor(const PageFile& file,
                     std::string table_name,
                     std::uint64_t table_definition_page_number,
                     std::uint32_t root_page_number,
                     std::optional<std::vector<std::uint8_t>> start_key = std::nullopt,
                     std::optional<std::vector<std::uint8_t>> stop_key = std::nullopt,
                     bool reverse = false);
    ~IndexEntryCursor();

    IndexEntryCursor(IndexEntryCursor&& other) noexcept;
    IndexEntryCursor& operator=(IndexEntryCursor&& other) noexcept;

    IndexEntryCursor(const IndexEntryCursor&) = delete;
    IndexEntryCursor& operator=(const IndexEntryCursor&) = delete;

    std::optional<IndexRowLocation> next();
    std::optional<IndexEntryRecord> next_entry();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
