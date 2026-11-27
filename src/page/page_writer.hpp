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
// `page_count`/`page_size` describe the file before the batch; recorded originals let rollback
// restore overwritten pages and truncate pages appended by the batch.
class PageWriteUndo {
public:
    PageWriteUndo(std::filesystem::path path, std::uint64_t page_count, std::uint32_t page_size);
    PageWriteUndo(const PageWriteUndo&) = delete;
    PageWriteUndo& operator=(const PageWriteUndo&) = delete;

    // True once at least one page has been recorded (i.e. there is something to roll back).
    bool dirty() const noexcept;
    // Restores every recorded page image and truncates appended pages, undoing the batch.
    void rollback();

private:
    friend class PageFileWriter;

    std::filesystem::path path_;                 // Database being edited.
    std::uint64_t original_page_count_;          // Page count before the batch.
    std::uint32_t page_size_;                    // Page size in bytes.
    std::map<std::uint64_t, std::vector<std::uint8_t>> originals_; // Before-image per overwritten page.
    bool appended_ = false;                      // True when the batch appended pages.
    // Created on the first recorded page; keeps the batch replayable after a process crash.
    std::unique_ptr<WriteJournal> journal_;
};

// Low-level page writer: whole-page overwrite, append, and flush. Used by the Jet4 write paths.
class PageFileWriter {
public:
    // Opens `path` for writing; when `undo` is given, every overwrite records its before-image.
    static PageFileWriter open(const std::filesystem::path& path, PageWriteUndo* undo = nullptr);

    PageFileWriter(const PageFileWriter&) = delete;
    PageFileWriter& operator=(const PageFileWriter&) = delete;
    PageFileWriter(PageFileWriter&&) = delete;
    PageFileWriter& operator=(PageFileWriter&&) = delete;

    // Path of the file being written.
    const std::filesystem::path& path() const noexcept;
    // Format metadata of the file.
    const FormatInfo& format() const noexcept;
    // Current page count of the file.
    std::uint64_t page_count() const;

    // Overwrites an existing page; `page` must be exactly one page long.
    void write_page(std::uint64_t page_number, const std::vector<std::uint8_t>& page);
    // Appends `page` at the end of the file and returns its new page number.
    std::uint64_t append_page(const std::vector<std::uint8_t>& page);
    // Flushes buffered writes to disk.
    void flush();

private:
    PageFileWriter(std::filesystem::path path,
                   std::fstream stream,
                   FormatInfo format,
                   std::uint64_t page_count,
                   PageWriteUndo* undo);

    // Throws when a prior write failed and left the handle in an unusable state.
    void ensure_usable() const;
    void fail_io(const char* message, std::uint64_t page_number);

    std::filesystem::path path_;   // File being written.
    std::fstream stream_;          // Open output stream.
    FormatInfo format_;            // Page size and format.
    std::uint64_t page_count_;     // Current page count.
    PageWriteUndo* undo_;          // Optional before-image recorder.
    mutable std::mutex mutex_;     // Serializes writes within this handle.
    bool unusable_ = false;        // Set after an unrecoverable write failure.
};

}
