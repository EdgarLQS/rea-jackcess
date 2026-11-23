#include "rea_jackcess/database.hpp"

#include "page/page_file.hpp"
#include "page/page_writer.hpp"
#include "page/write_journal.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rea_jackcess::Database;
using rea_jackcess::DatabaseError;
using rea_jackcess::DatabaseErrorCode;
using rea_jackcess::DatabaseFormat;
using rea_jackcess::DatabaseOpenMode;
using rea_jackcess::DataType;
using rea_jackcess::Table;
using rea_jackcess::TableBuilder;
namespace detail = rea_jackcess::detail;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("rea-jackcess-recovery-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    std::filesystem::path file(const std::string& name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read " + path.string());
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

Table find_table(const Database& database, const std::string& name) {
    for (auto table : database.tables()) {
        if (table.is_user_table() && table.name() == name) {
            return table;
        }
    }
    throw std::runtime_error("missing table " + name);
}

// Rewrites one page and abandons the batch without flushing, which is what a process that dies
// mid-write leaves behind: a journal plus whatever the operating system already stored.
void abandon_page_write(const std::filesystem::path& path, std::uint64_t page_number) {
    auto file = detail::PageFile::open(path);
    detail::PageWriteUndo undo(path, file.page_count(), file.format().page_size);
    auto writer = detail::PageFileWriter::open(path, &undo);
    auto page = file.read_page(page_number);
    for (auto& byte : page) {
        byte = 0xab;
    }
    writer.write_page(page_number, page);
}

void abandon_append(const std::filesystem::path& path) {
    auto file = detail::PageFile::open(path);
    detail::PageWriteUndo undo(path, file.page_count(), file.format().page_size);
    auto writer = detail::PageFileWriter::open(path, &undo);
    std::vector<std::uint8_t> page(file.format().page_size, 0xcd);
    writer.append_page(page);
}

void create_database(const std::filesystem::path& path, DatabaseFormat format) {
    Database::create(path, format,
                     {TableBuilder("Recovered").add_column("Id", DataType::int32)});
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    auto table = find_table(database, "Recovered");
    table.add_row({std::int32_t{1}});
}

void test_committed_write_leaves_no_journal(const std::filesystem::path& path) {
    check(!detail::WriteJournal::pending(path),
          "a completed write should not leave a recovery journal behind");
}

void test_page_rollback(DatabaseFormat format, const std::filesystem::path& path) {
    create_database(path, format);
    test_committed_write_leaves_no_journal(path);
    const auto original = read_bytes(path);

    auto file = detail::PageFile::open(path);
    const auto victim = file.page_count() - 1;
    const auto original_page = file.read_page(victim);
    abandon_page_write(path, victim);

    check(detail::WriteJournal::pending(path), "the abandoned write did not leave a journal");
    check(read_bytes(path) != original, "the abandoned write should have changed the file");
    check(detail::PageFile::open(path).read_page(victim) != original_page,
          "the abandoned write should have replaced the page");

    bool refused = false;
    try {
        auto database = Database::open(path);
        (void)database;
    } catch (const DatabaseError& error) {
        refused = error.code() == DatabaseErrorCode::io_error;
    }
    check(refused, "a read-only open must refuse a database with a pending journal");
    check(detail::WriteJournal::pending(path),
          "the refused read-only open must not consume the journal");

    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        (void)database;
    }
    check(!detail::WriteJournal::pending(path), "recovery did not remove the journal");
    check(read_bytes(path) == original, "recovery did not restore the original file bytes");

    // The recovered database must be usable, which also proves the handle was not left locked.
    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        auto table = find_table(database, "Recovered");
        table.add_row({std::int32_t{2}});
        check(table.row_count_header() == 2, "writing after recovery failed");
    }
}

void test_append_truncation(DatabaseFormat format, const std::filesystem::path& path) {
    create_database(path, format);
    const auto original = read_bytes(path);
    const auto original_pages = original.size();

    abandon_append(path);
    check(detail::WriteJournal::pending(path), "the abandoned append did not leave a journal");
    check(read_bytes(path).size() > original_pages, "the abandoned append should have grown the file");

    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        (void)database;
    }
    check(read_bytes(path) == original, "recovery did not truncate the appended page");
    check(!detail::WriteJournal::pending(path), "recovery did not remove the append journal");
}

void test_create_replaces_stale_journal(DatabaseFormat format, const std::filesystem::path& path) {
    create_database(path, format);
    abandon_page_write(path, 1);
    check(detail::WriteJournal::pending(path), "the abandoned write did not leave a journal");
    // Recreating the database must not replay images recorded for the file being replaced.
    create_database(path, format);
    check(!detail::WriteJournal::pending(path), "recreating the database left the old journal");
    auto database = Database::open(path, DatabaseOpenMode::read_write);
    auto table = find_table(database, "Recovered");
    check(table.row_count_header() == 1, "the recreated database has an unexpected row count");
}

int parse_format(const std::string& text) { return text == "v2003" ? 1 : 0; }

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: crash_recovery_test <v2000|v2003>\n";
        return 2;
    }
    TemporaryDirectory directory;
    const auto format = parse_format(argv[1]) == 1 ? DatabaseFormat::v2003 : DatabaseFormat::v2000;
    try {
        test_page_rollback(format, directory.file("rollback.mdb"));
        test_append_truncation(format, directory.file("append.mdb"));
        test_create_replaces_stale_journal(format, directory.file("recreate.mdb"));
        std::cout << "crash recovery tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "crash recovery tests failed: " << error.what() << '\n';
        return 1;
    }
}
