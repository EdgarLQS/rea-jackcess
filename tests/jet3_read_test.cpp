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

        // The catalog now enumerates Jet3 tables and decodes their rows.
        Database database = Database::open(path);
        check(database.format().version == EngineVersion::jet3, "Database reports a different version");
        const auto& tables = database.tables();
        std::vector<std::string> names;
        for (const auto& table : tables) {
            names.push_back(table.name());
        }
        const std::vector<std::string> expected = {"MSysObjects", "MSysACEs", "MSysQueries",
                                                   "MSysRelationships", "MSysModules2",
                                                   "MSysModules", "Table1", "Table2", "Table3",
                                                   "Table4"};
        check(names == expected, "Jet3 table list changed");

        const auto table1 = std::find_if(tables.begin(), tables.end(), [](const auto& table) {
            return table.name() == "Table1";
        });
        check(table1 != tables.end(), "Table1 is missing");
        check(table1->columns().size() == 9, "Table1 column count changed");
        check(table1->row_count_header() == 2, "Table1 row count changed");

        std::vector<rea_jackcess::Row> rows;
        auto cursor = table1->rows();
        while (auto row = cursor.next()) {
            rows.push_back(*row);
        }
        check(rows.size() == 2, "Table1 live row count changed");

        check(std::get<std::string>(rows[0].at(0)) == "a", "Table1 row 1 column A changed");
        check(std::get<std::string>(rows[0].at(1)) == "b", "Table1 row 1 column B changed");
        check(std::get<std::int8_t>(rows[0].at(2)) == 0, "Table1 row 1 column C changed");
        check(std::get<std::int16_t>(rows[0].at(3)) == 0, "Table1 row 1 column D changed");
        check(std::get<std::int32_t>(rows[0].at(4)) == 0, "Table1 row 1 column E changed");
        check(std::get<double>(rows[0].at(5)) == 0.0, "Table1 row 1 column F changed");
        check(std::get<rea_jackcess::DateTimeValue>(rows[0].at(6)).iso8601 == "1981-12-12T00:00",
              "Table1 row 1 column G changed");
        check(std::get<rea_jackcess::DecimalValue>(rows[0].at(7)).text == "0.0000",
              "Table1 row 1 column H changed");
        check(std::get<bool>(rows[0].at(8)) == false, "Table1 row 1 column I changed");

        check(std::get<std::string>(rows[1].at(0)) == "abcdefg", "Table1 row 2 column A changed");
        check(std::get<std::string>(rows[1].at(1)) == "hijklmnop", "Table1 row 2 column B changed");
        check(std::get<std::int8_t>(rows[1].at(2)) == 2, "Table1 row 2 column C changed");
        check(std::get<std::int16_t>(rows[1].at(3)) == 222, "Table1 row 2 column D changed");
        check(std::get<std::int32_t>(rows[1].at(4)) == 333333333, "Table1 row 2 column E changed");
        check(std::get<double>(rows[1].at(5)) == 444.555, "Table1 row 2 column F changed");
        check(std::get<rea_jackcess::DateTimeValue>(rows[1].at(6)).iso8601 == "1974-09-21T00:00",
              "Table1 row 2 column G changed");
        check(std::get<rea_jackcess::DecimalValue>(rows[1].at(7)).text == "3.5000",
              "Table1 row 2 column H changed");
        check(std::get<bool>(rows[1].at(8)) == true, "Table1 row 2 column I changed");

        check(read_bytes(path) == before, "a read-only Jet3 open modified the sample");
        std::cout << "Jet3 read checks passed: " << path.filename().string() << " ("
                  << file.page_count() << " pages, catalog still unsupported)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Jet3 read check failed: " << error.what() << '\n';
        return 1;
    }
}
