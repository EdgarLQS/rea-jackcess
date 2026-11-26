#include "rea_jackcess/database.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rea_jackcess::Database;
using rea_jackcess::Table;

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

Table find_table(const Database& database, const std::string& name) {
    for (const auto& table : database.tables()) {
        if (table.name() == name) {
            return table;
        }
    }
    throw std::runtime_error("fixture table is missing: " + name);
}

std::vector<std::int32_t> scan_ids(rea_jackcess::RowCursor cursor) {
    std::vector<std::int32_t> ids;
    while (auto row = cursor.next()) {
        ids.push_back(std::get<std::int32_t>(row->at(0)));
    }
    return ids;
}

}  // namespace

// Index definitions and the index cursor for Jet3, checked against the fixed Jackcess 5.2.0
// index order on indexV1997.mdb (Table1 primary key: 0, 1, 2, 3).
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: jet3_index_read <indexV1997.mdb>\n";
        return 2;
    }
    const std::filesystem::path path(argv[1]);
    if (!std::filesystem::exists(path)) {
        std::cout << "Jet3 sample is missing: " << path << '\n';
        return 77;
    }
    try {
        const std::string before = read_bytes(path);
        auto database = Database::open(path);
        Table table = find_table(database, "Table1");

        std::vector<std::string> names;
        for (const auto& index : table.indexes()) {
            names.push_back(index.name);
        }
        const std::vector<std::string> expected = {"id", "PrimaryKey", "Table2Table1", "Table3Table1"};
        check(names == expected, "Jet3 index list changed");

        const auto primary = std::find_if(table.indexes().begin(), table.indexes().end(),
                                          [](const auto& index) { return index.name == "PrimaryKey"; });
        check(primary != table.indexes().end(), "PrimaryKey is missing");
        check(primary->primary_key && primary->unique, "PrimaryKey flags changed");
        check(primary->root_page_number == 40, "PrimaryKey root page changed");
        check(primary->columns.size() == 1 && primary->columns[0].name == "id",
              "PrimaryKey columns changed");

        check(scan_ids(table.rows_by_index("PrimaryKey")) == std::vector<std::int32_t>({0, 1, 2, 3}),
              "Jet3 primary key scan order changed");
        check(scan_ids(table.rows_by_index_reverse("PrimaryKey")) == std::vector<std::int32_t>({3, 2, 1, 0}),
              "Jet3 primary key reverse scan order changed");

        const auto found = table.find_row_by_index("PrimaryKey", std::int32_t{2});
        check(found.has_value() && std::get<std::int32_t>(found->at(0)) == 2,
              "Jet3 primary key lookup changed");

        check(read_bytes(path) == before, "reading Jet3 indexes modified the sample");
        std::cout << "Jet3 index checks passed: " << path.filename().string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Jet3 index check failed: " << error.what() << '\n';
        return 1;
    }
}
