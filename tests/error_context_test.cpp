// Fixes the read-side error taxonomy callers may rely on: which DatabaseErrorCode each failure
// category uses and which context fields are filled. Cases that need a real ACE sample read the
// committed fixture and are skipped when it is absent.

#include "rea_jackcess/csv.hpp"
#include "rea_jackcess/database.hpp"
#include "rea_jackcess/database_file.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rea_jackcess::Database;
using rea_jackcess::DatabaseError;
using rea_jackcess::DatabaseErrorCode;
using rea_jackcess::DatabaseFile;
using rea_jackcess::DatabaseOpenMode;
using rea_jackcess::Table;

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
                ("rea-jackcess-errors-" + std::to_string(stamp));
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

void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(static_cast<bool>(output), "could not write " + path.string());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

template <typename Callable>
DatabaseError expect_error(Callable&& callable, const std::string& what) {
    try {
        callable();
    } catch (const DatabaseError& error) {
        return error;
    }
    throw std::runtime_error(what + ": no error was reported");
}

// A missing file is an I/O problem, not a format problem.
void test_io_error(const TemporaryDirectory& directory) {
    const auto path = directory.file("missing.mdb");
    const auto error = expect_error([&] { (void)Database::open(path); }, "a missing database");
    check(error.code() == DatabaseErrorCode::io_error, "a missing file should report io_error");
    check(error.path() == path, "io_error should name the database path");
    check(!error.page_number(), "a missing file has no page to report");
}

// A file shorter than the fixed header is truncated rather than unreadable.
void test_truncated_file(const TemporaryDirectory& directory) {
    const auto path = directory.file("short.mdb");
    write_file(path, std::vector<std::uint8_t>(10, 0));
    const auto error = expect_error([&] { (void)Database::open(path); }, "a 10-byte database");
    check(error.code() == DatabaseErrorCode::truncated_file,
          "a file shorter than its header should report truncated_file");
    check(error.path() == path, "truncated_file should name the database path");

    // The same bytes opened from memory report the handle name instead of a path.
    const auto memory_error = expect_error(
        [&] { (void)Database::open(std::vector<std::uint8_t>(10, 0)); }, "a 10-byte buffer");
    check(memory_error.code() == DatabaseErrorCode::truncated_file,
          "a short buffer should report truncated_file");
    check(memory_error.path() == "<memory>", "a memory open should report <memory>");
}

void test_unsupported_version(const TemporaryDirectory& directory) {
    const auto path = directory.file("future.mdb");
    std::vector<std::uint8_t> bytes(4096, 0);
    bytes[20] = 9;
    write_file(path, bytes);
    const auto error = expect_error([&] { (void)Database::open(path); }, "engine version 9");
    check(error.code() == DatabaseErrorCode::unsupported_version,
          "an unknown engine version should report unsupported_version");
    check(error.path() == path, "unsupported_version should name the database path");
    check(std::string(error.what()).find("9") != std::string::npos,
          "the version error should include the unexpected version byte");
}

void test_misaligned_file(const TemporaryDirectory& directory) {
    const auto path = directory.file("misaligned.mdb");
    std::vector<std::uint8_t> bytes(4097, 0);
    bytes[20] = 1;
    write_file(path, bytes);
    const auto error = expect_error([&] { (void)Database::open(path); }, "an unaligned file");
    check(error.code() == DatabaseErrorCode::corrupted_file,
          "a size that is not a multiple of the page size should report corrupted_file");
    check(error.path() == path, "corrupted_file should name the database path");
}

void test_page_out_of_range(const TemporaryDirectory& directory) {
    const auto path = directory.file("range.mdb");
    Database::create(path, rea_jackcess::DatabaseFormat::v2000);
    const DatabaseFile file = DatabaseFile::open(path);
    const auto error = expect_error([&] { (void)file.read_page(file.page_count()); },
                                    "a page past the end");
    check(error.code() == DatabaseErrorCode::page_out_of_range,
          "a page number past the end should report page_out_of_range");
    check(error.path() == path, "page_out_of_range should name the database path");
    check(error.page_number() == file.page_count(), "the error should name the requested page");
}

// ACE is read-only, and the refusal carries the path so a caller can report it.
void test_unsupported_operation(const std::filesystem::path& ace_fixture) {
    if (!std::filesystem::exists(ace_fixture)) {
        std::cout << "ACE fixture is missing: " << ace_fixture << '\n';
        return;
    }
    const auto error = expect_error(
        [&] { (void)Database::open(ace_fixture, DatabaseOpenMode::read_write); },
        "an ACE read-write open");
    check(error.code() == DatabaseErrorCode::unsupported_operation,
          "an ACE read-write open should report unsupported_operation");
    check(error.path() == ace_fixture, "the refusal should name the database path");

    // A column the CSV reader cannot fill reports the table and column, not just a message.
    auto database = Database::open(ace_fixture);
    const auto tables = database.tables();
    const auto found = std::find_if(tables.begin(), tables.end(), [](const Table& candidate) {
        return candidate.is_user_table() && candidate.name() == "Table1";
    });
    check(found != tables.end(), "the ACE fixture should hold Table1");
    Table table = *found;
    std::istringstream input;
    const auto import_error = expect_error(
        [&] { (void)rea_jackcess::import_table_csv(table, input); }, "a complex-column CSV import");
    check(import_error.code() == DatabaseErrorCode::unsupported_operation,
          "importing into a complex column should report unsupported_operation");
    check(import_error.context().table_name == "Table1",
          "the import refusal should name the table");
    check(import_error.context().column_name.has_value(),
          "the import refusal should name the column");
}

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: error_context <complex-data-v2007.accdb>\n";
        return 2;
    }
    try {
        const TemporaryDirectory directory;
        test_io_error(directory);
        test_truncated_file(directory);
        test_unsupported_version(directory);
        test_misaligned_file(directory);
        test_page_out_of_range(directory);
        test_unsupported_operation(argv[1]);
        std::cout << "read error categories hold\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
