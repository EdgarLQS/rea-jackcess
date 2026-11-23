#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace rea_jackcess::detail {

// Write-ahead journal for one page-write batch. Every page image is appended and flushed to
// `<database>.reajournal` before the database page itself is overwritten, so a process that dies
// mid-batch can be rolled back the next time the database is opened. The journal is removed once
// the batch has been flushed to the database.
//
// The file records the page size and the page count the batch started with; replay restores the
// recorded images and truncates the database back to that count, which also undoes appended pages.
class WriteJournal {
public:
    static std::filesystem::path path_for(const std::filesystem::path& database);

    // Returns true when a journal file is present, i.e. a previous batch did not finish.
    static bool pending(const std::filesystem::path& database);

    // Replays the journal into the database and removes it. A trailing partial record is ignored:
    // records are flushed before the pages they describe are written, so an incomplete tail means
    // those pages were never touched.
    static void recover(const std::filesystem::path& database);

    WriteJournal(const std::filesystem::path& database,
                 std::uint64_t page_count,
                 std::uint32_t page_size);
    ~WriteJournal();

    WriteJournal(const WriteJournal&) = delete;
    WriteJournal& operator=(const WriteJournal&) = delete;

    // Records the pre-image of a page that is about to be overwritten.
    void record_page(std::uint64_t page_number, const std::vector<std::uint8_t>& bytes);
    // Marks that the batch appends pages, so replay truncates them away.
    void record_append();
    // Removes the journal after the batch has been flushed.
    void commit();

private:
    std::filesystem::path path_;
    std::uint32_t page_size_ = 0;
    int fd_ = -1;
};

}
