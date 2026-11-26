#pragma once

#include "rea_jackcess/database_file.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace rea_jackcess::detail {

class PageFile {
public:
    static PageFile open(const std::filesystem::path& path);

    // Opens a database that already lives in memory. The buffer must hold the complete file;
    // errors report `memory_name` instead of a path.
    static PageFile open(std::vector<std::uint8_t> bytes,
                         std::string memory_name = "<memory>");

    PageFile(PageFile&& other);
    PageFile& operator=(PageFile&& other);

    PageFile(const PageFile&) = delete;
    PageFile& operator=(const PageFile&) = delete;

    const std::filesystem::path& path() const noexcept;
    const FormatInfo& format() const noexcept;
    std::uint64_t page_count() const noexcept;
    std::vector<std::uint8_t> read_page(std::uint64_t page_number) const;

private:
    PageFile(std::filesystem::path path,
             std::ifstream stream,
             FormatInfo format,
             std::uint64_t file_size);

    PageFile(std::filesystem::path path,
             std::vector<std::uint8_t> bytes,
             FormatInfo format);

    std::filesystem::path path_;
    mutable std::ifstream stream_;
    FormatInfo format_;
    std::uint64_t file_size_;
    mutable std::mutex stream_mutex_;
    // Set only for memory-backed files; read_page then serves slices of this buffer.
    std::shared_ptr<const std::vector<std::uint8_t>> memory_;
};

}
