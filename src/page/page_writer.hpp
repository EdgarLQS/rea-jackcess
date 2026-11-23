#pragma once

#include "rea_jackcess/database_file.hpp"

#include "write_journal.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

namespace rea_jackcess::detail {

// In-memory before-images for a write operation. Roll back only after its writers close.
class PageWriteUndo {
public:
    PageWriteUndo(std::filesystem::path path, std::uint64_t page_count, std::uint32_t page_size);
    PageWriteUndo(const PageWriteUndo&) = delete;
    PageWriteUndo& operator=(const PageWriteUndo&) = delete;

    bool dirty() const noexcept;
    void rollback();

private:
    friend class PageFileWriter;

    std::filesystem::path path_;
    std::uint64_t original_page_count_;
    std::uint32_t page_size_;
    std::map<std::uint64_t, std::vector<std::uint8_t>> originals_;
    bool appended_ = false;
    // Created on the first recorded page; keeps the batch replayable after a process crash.
    std::unique_ptr<WriteJournal> journal_;
};

class PageFileWriter {
public:
    static PageFileWriter open(const std::filesystem::path& path, PageWriteUndo* undo = nullptr);

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
                   std::uint64_t page_count,
                   PageWriteUndo* undo);

    void ensure_usable() const;
    void fail_io(const char* message, std::uint64_t page_number);

    std::filesystem::path path_;
    std::fstream stream_;
    FormatInfo format_;
    std::uint64_t page_count_;
    PageWriteUndo* undo_;
    mutable std::mutex mutex_;
    bool unusable_ = false;
};

}
