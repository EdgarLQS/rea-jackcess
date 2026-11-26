#include "catalog/jet3_table_definition.hpp"
#include "page/page_file.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace detail = rea_jackcess::detail;

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

const detail::ColumnDefinition& column(const detail::TableDefinition& definition,
                                       std::size_t index) {
    check(index < definition.columns.size(), "column index is out of range");
    return definition.columns[index];
}

}  // namespace

// The layout is taken from the fixed Jackcess 5.2.0 JetFormat.Jet3Format; expectations come from
// JackcessTableDump on the same file (Table1 columns A..I, MSysObjects 17 columns).
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: jet3_table_definition <jet3-sample.mdb>\n";
        return 2;
    }
    const std::filesystem::path path(argv[1]);
    if (!std::filesystem::exists(path)) {
        std::cout << "Jet3 sample is missing: " << path << '\n';
        return 77;
    }
    try {
        const std::string before = read_bytes(path);
        const auto file = detail::PageFile::open(path);

        // Table1: nine columns, two rows.
        const auto table1 = detail::read_jet3_table_definition(file, 29);
        check(table1.row_count == 2, "Table1 row count changed");
        check(table1.column_count == 9, "Table1 column count changed");
        check(table1.columns.size() == 9, "Table1 columns were not parsed");
        const std::vector<std::string> names = {"A", "B", "C", "D", "E", "F", "G", "H", "I"};
        const std::vector<std::uint8_t> types = {0x0a, 0x0a, 0x02, 0x03, 0x04,
                                                 0x07, 0x08, 0x05, 0x01};
        const std::vector<std::uint16_t> lengths = {50, 100, 1, 2, 4, 8, 8, 8, 1};
        for (std::size_t index = 0; index < names.size(); ++index) {
            check(column(table1, index).name == names[index],
                  "Table1 column name " + std::to_string(index) + " changed");
            check(column(table1, index).type == types[index],
                  "Table1 column type " + names[index] + " changed");
            check(column(table1, index).length == lengths[index],
                  "Table1 column length " + names[index] + " changed");
            check(column(table1, index).column_number == index,
                  "Table1 column number " + names[index] + " changed");
        }

        // MSysObjects: 22 rows, 17 columns, first four names fixed.
        const auto objects = detail::read_jet3_table_definition(file, 2);
        check(objects.row_count == 22, "MSysObjects row count changed");
        check(objects.column_count == 17, "MSysObjects column count changed");
        check(column(objects, 0).name == "Id" && column(objects, 0).type == 0x04,
              "MSysObjects Id column changed");
        check(column(objects, 1).name == "ParentId" && column(objects, 1).type == 0x04,
              "MSysObjects ParentId column changed");
        check(column(objects, 2).name == "Name" && column(objects, 2).type == 0x0a,
              "MSysObjects Name column changed");
        check(column(objects, 3).name == "Type" && column(objects, 3).type == 0x03,
              "MSysObjects Type column changed");

        // Table2 spans several definition pages; the continuation chain must stitch 89 columns.
        bool found_wide_table = false;
        for (std::uint64_t page = 1; page < file.page_count() && !found_wide_table; ++page) {
            try {
                const auto definition = detail::read_jet3_table_definition(file, page);
                if (definition.column_count != 89) {
                    continue;
                }
                found_wide_table = true;
                check(definition.columns.front().name == "column1", "wide table first column changed");
                check(definition.columns.back().name == "column89", "wide table last column changed");
                check(definition.continuation_page_count >= 1,
                      "wide table should continue on a second definition page");
            } catch (const rea_jackcess::DatabaseError&) {
                // Pages that are not table definitions are expected while scanning.
            }
        }
        check(found_wide_table, "the 89-column Jet3 table was not found");

        check(read_bytes(path) == before, "reading Jet3 definitions modified the sample");
        std::cout << "Jet3 table definition checks passed: " << path.filename().string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Jet3 table definition check failed: " << error.what() << '\n';
        return 1;
    }
}
