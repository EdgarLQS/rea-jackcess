#include "rea_jackcess/database.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rea_jackcess::Database;
using rea_jackcess::FileFormat;
using rea_jackcess::Table;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

FileFormat parse_format(const std::string& name) {
    if (name == "v2000") {
        return FileFormat::v2000;
    }
    if (name == "v2003") {
        return FileFormat::v2003;
    }
    if (name == "v2007") {
        return FileFormat::v2007;
    }
    throw std::runtime_error("unknown expected format " + name);
}

const Table& require_table(const std::vector<Table>& tables, const std::string& name) {
    const auto table = std::find_if(tables.begin(), tables.end(), [&](const auto& candidate) {
        return candidate.name() == name;
    });
    check(table != tables.end(), "sample does not expose table " + name);
    return *table;
}

std::size_t column_index(const Table& table, const std::string& name) {
    const auto& columns = table.columns();
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (columns[index].name == name) {
            return index;
        }
    }
    throw std::runtime_error("sample table " + table.name() + " is missing column " + name);
}

std::vector<bool> read_booleans(const Table& table, std::size_t index, std::size_t expected_rows) {
    std::vector<bool> values;
    auto cursor = table.rows();
    while (const auto row = cursor.next()) {
        values.push_back(std::get<bool>(row->at(index)));
    }
    check(values.size() == expected_rows, "BOOLEAN column returned an unexpected row count");
    return values;
}

}

// Reads an Access-generated copy of the tracked PGeo sample and verifies that the saved version
// is classified and read like the original data. The copies are local-only and gitignored; CMake
// registers this executable only for files that exist at configure time.
int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: rea_jackcess_access_version_tests <sample> <v2000|v2003|v2007>\n";
            return 2;
        }
        const std::filesystem::path path = argv[1];
        const auto expected = parse_format(argv[2]);

        const auto database = Database::open(path);
        check(database.file_format() == expected, "Access-generated sample has an unexpected format");

        const auto tables = database.tables();
        const auto user_tables = static_cast<std::size_t>(
            std::count_if(tables.begin(), tables.end(), [](const auto& table) {
                return table.is_user_table();
            }));
        check(user_tables == 43, "Access-generated sample changed its user table count");

        const auto& geometry_columns = require_table(tables, "GDB_GeomColumns");
        check(geometry_columns.row_count_header() == 18,
              "Access-generated GDB_GeomColumns changed its row count");
        const auto has_z = column_index(geometry_columns, "HasZ");
        const auto has_m = column_index(geometry_columns, "HasM");
        std::vector<bool> expected_has_z(18, false);
        expected_has_z[15] = true;
        expected_has_z[16] = true;
        expected_has_z[17] = true;
        check(read_booleans(geometry_columns, has_z, 18) == expected_has_z,
              "Access-generated HasZ BOOLEAN bits changed");
        check(read_booleans(geometry_columns, has_m, 18) == std::vector<bool>(18, false),
              "Access-generated HasM BOOLEAN bits changed");

        check(require_table(tables, "GDB_ItemTypes").row_count_header() == 34,
              "Access-generated GDB_ItemTypes changed its row count");
        check(require_table(tables, "GDB_SpatialRefs").row_count_header() == 3,
              "Access-generated GDB_SpatialRefs changed its row count");

        std::cout << "Access-generated " << argv[2] << " sample passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Access-generated sample read failed: " << error.what() << '\n';
        return 1;
    }
}
