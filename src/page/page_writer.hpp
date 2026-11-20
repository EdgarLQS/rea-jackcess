#pragma once

#include "rea_jackcess/database_file.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace rea_jackcess::detail {

class PageFileWriter {
public:
    static PageFileWriter open(const std::filesystem::path& path);

    PageFileWriter(const PageFileWriter&) = delete;
    PageFileWriter& operator=(const PageFileWriter&) = delete;
    PageFileWriter(PageFileWriter&&) = delete;
    PageFileWriter& operator=(PageFileWriter&&) = delete;

    const std::filesystem::path& path() const noexcept;
    const FormatInfo& format() const noexcept;
    std::uint64_t page_count() const;

    void write_page(std::uint64_t page_number, const std::vector<std::uint8_t>& page);
    std::uint64_t append_page(const std::vector<std::uint8_t>& page);
    void flush();

private:
    PageFileWriter(std::filesystem::path path,
                   std::fstream stream,
                   FormatInfo format,
                   std::uint64_t page_count);

    void ensure_usable() const;
    void fail_io(const char* message, std::uint64_t page_number);

    std::filesystem::path path_;
    std::fstream stream_;
    FormatInfo format_;
    std::uint64_t page_count_;
    mutable std::mutex mutex_;
    bool unusable_ = false;
};

}
