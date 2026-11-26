#include "rea_jackcess/database.hpp"
#include "rea_jackcess/database_file.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

using rea_jackcess::Database;
using rea_jackcess::DatabaseError;
using rea_jackcess::DatabaseErrorCode;
using rea_jackcess::DatabaseFile;
using rea_jackcess::EngineVersion;
using rea_jackcess::FileFormat;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "could not read the Jet3 sample");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

// Jet3 read-only support is staged: format recognition and page geometry are covered here, while
// catalog and row decoding still report an explicit unsupported version. The test pins that
// boundary so the next stage cannot silently regress it.
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: jet3_read <jet3-sample.mdb>\n";
        return 2;
    }
    const std::filesystem::path path(argv[1]);
    if (!std::filesystem::exists(path)) {
        std::cout << "Jet3 sample is missing: " << path << '\n';
        return 77;
    }
    try {
        const std::string before = read_bytes(path);

        const auto file = DatabaseFile::open(path);
        check(file.format().version == EngineVersion::jet3, "sample is not recognised as Jet3");
        check(file.file_format() == FileFormat::v1997, "unexpected Jet3 file format value");
        check(file.format().page_size == 2048, "Jet3 page size must be 2048");
        check(file.format().read_only, "Jet3 must be reported read-only");
        check(file.page_count() > 2, "sample has no pages beyond the header and catalog");

        // The staged boundary: opening a Jet3 database still stops at catalog enumeration.
        const auto expect_unsupported = [&](const std::string& label) {
            try {
                Database database = Database::open(path);
                (void)database.tables();
            } catch (const DatabaseError& error) {
                check(error.code() == DatabaseErrorCode::unsupported_version,
                      label + ": unexpected error code for a Jet3 database");
                check(std::string(error.what()).find("unsupported engine version") != std::string::npos,
                      label + ": the diagnostic no longer names the engine version");
                check(error.path() == path, label + ": the error lost the file path");
                return;
            }
            throw std::runtime_error(label + ": a Jet3 catalog read was accepted");
        };
        expect_unsupported("Jet3 open");

        check(read_bytes(path) == before, "a read-only Jet3 open modified the sample");
        std::cout << "Jet3 read checks passed: " << path.filename().string() << " ("
                  << file.page_count() << " pages, catalog still unsupported)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Jet3 read check failed: " << error.what() << '\n';
        return 1;
    }
}
