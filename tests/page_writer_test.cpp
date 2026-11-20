#include "page/page_writer.hpp"

#include "rea_jackcess/database_file.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

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
                ("rea-jackcess-page-writer-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    std::filesystem::path file(const std::string& name) const {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

void write_database_header(const std::filesystem::path& path, std::uint8_t version, std::size_t page_size) {
    std::vector<std::uint8_t> bytes(page_size * 2, 0);
    constexpr std::string_view engine_name = "Standard Jet DB";
    for (std::size_t index = 0; index < engine_name.size(); ++index) {
        bytes[4 + index] = static_cast<std::uint8_t>(engine_name[index]);
    }
    bytes[20] = version;

    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(output), "failed to write a database fixture");
}

template <typename Operation>
void expect_error(rea_jackcess::DatabaseErrorCode code, Operation operation) {
    try {
        operation();
    } catch (const rea_jackcess::DatabaseError& error) {
        check(error.code() == code, "database operation returned the wrong error code");
        return;
    }
    throw std::runtime_error("expected database operation to fail");
}

void test_existing_and_appended_pages(const TemporaryDirectory& directory) {
    using rea_jackcess::DatabaseErrorCode;
    using rea_jackcess::detail::PageFileWriter;

    const auto path = directory.file("jet4.mdb");
    write_database_header(path, 1, 4096);

    auto writer = PageFileWriter::open(path);
    check(writer.format().version == rea_jackcess::EngineVersion::jet4,
          "writer did not preserve the Jet4 format information");
    check(writer.page_count() == 2, "writer reported the wrong initial page count");

    bool invalid_size_rejected = false;
    try {
        writer.write_page(1, std::vector<std::uint8_t>(4095, 0x11));
    } catch (const std::invalid_argument&) {
        invalid_size_rejected = true;
    }
    check(invalid_size_rejected, "writer accepted a short page");
    expect_error(DatabaseErrorCode::page_out_of_range, [&] {
        writer.write_page(2, std::vector<std::uint8_t>(4096, 0x11));
    });

    const std::vector<std::uint8_t> replacement(4096, 0xa5);
    writer.write_page(1, replacement);
    const std::vector<std::uint8_t> appended(4096, 0xc3);
    check(writer.append_page(appended) == 2, "writer returned the wrong appended page number");
    check(writer.page_count() == 3, "writer did not advance the page count after append");
    writer.flush();

    auto database = rea_jackcess::DatabaseFile::open(path);
    check(database.page_count() == 3, "reopened database did not include the appended page");
    check(database.read_page(1) == replacement, "overwritten page did not persist");
    check(database.read_page(2) == appended, "appended page did not persist");
}

void test_unsupported_formats(const TemporaryDirectory& directory) {
    using rea_jackcess::DatabaseErrorCode;
    using rea_jackcess::detail::PageFileWriter;

    const auto path = directory.file("jet3.mdb");
    write_database_header(path, 0, 2048);
    expect_error(DatabaseErrorCode::unsupported_operation, [&] {
        auto writer = PageFileWriter::open(path);
        (void)writer;
    });

    const auto ace_path = directory.file("ace2007.accdb");
    write_database_header(ace_path, 2, 4096);
    expect_error(DatabaseErrorCode::unsupported_operation, [&] {
        auto writer = PageFileWriter::open(ace_path);
        (void)writer;
    });

    const auto unaligned_path = directory.file("unaligned.mdb");
    write_database_header(unaligned_path, 1, 4096);
    {
        std::ofstream output(unaligned_path, std::ios::binary | std::ios::app);
        output.put('\0');
        check(static_cast<bool>(output), "failed to write the unaligned database fixture");
    }
    expect_error(DatabaseErrorCode::corrupted_file, [&] {
        auto writer = PageFileWriter::open(unaligned_path);
        (void)writer;
    });
}

}

int main() {
    try {
        TemporaryDirectory directory;
        test_existing_and_appended_pages(directory);
        test_unsupported_formats(directory);
        std::cout << "page writer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "page writer tests failed: " << error.what() << '\n';
        return 1;
    }
}
