#include "rea_jackcess/database.hpp"

#include "util/sha256.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// Pinned PGeo sample kept outside version control; see plan/Jackcess-C++重写/04-P0基线核验记录.md.
constexpr const char* pinned_sample_sha256 =
    "0bc0acb9c732de1e17c43c2d0a6019f7b5a62ccf83c5818a1bc42db7dbc460c6";

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const rea_jackcess::Table& require_table(const std::vector<rea_jackcess::Table>& tables,
                                         const std::string& name) {
    const auto table = std::find_if(tables.begin(), tables.end(), [&](const auto& candidate) {
        return candidate.name() == name;
    });
    check(table != tables.end(), "real sample does not expose table " + name);
    return *table;
}

std::size_t column_index(const rea_jackcess::Table& table, const std::string& name) {
    const auto& columns = table.columns();
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (columns[index].name == name) {
            return index;
        }
    }
    throw std::runtime_error("real sample table is missing column " + name);
}

std::vector<bool> read_booleans(const rea_jackcess::Table& table,
                                std::size_t index,
                                std::size_t expected_rows) {
    std::vector<bool> values;
    auto cursor = table.rows();
    while (const auto row = cursor.next()) {
        values.push_back(std::get<bool>(row->at(index)));
    }
    check(values.size() == expected_rows,
          "real sample BOOLEAN column returned an unexpected row count");
    return values;
}

}

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "Usage: rea_jackcess_real_sample_tests <sample.mdb>\n";
            return 2;
        }
        const std::filesystem::path path = argv[1];
        check(rea_jackcess::detail::sha256_file_hex(path) == pinned_sample_sha256,
              "real sample hash does not match the pinned PGeo fixture");

        const auto database = rea_jackcess::Database::open(path);
        const auto tables = database.tables();

        {
            const auto& geometry_columns = require_table(tables, "GDB_GeomColumns");
            check(geometry_columns.row_count_header() == 18,
                  "real sample GDB_GeomColumns changed its row count");
            const std::size_t has_z = column_index(geometry_columns, "HasZ");
            const std::size_t has_m = column_index(geometry_columns, "HasM");
            const auto& has_z_column = geometry_columns.columns()[has_z];
            const auto& has_m_column = geometry_columns.columns()[has_m];
            check(has_z_column.type == rea_jackcess::DataType::boolean && has_z_column.fixed &&
                      has_z_column.size == 1 && has_z_column.nullable == std::optional<bool>(false),
                  "real sample HasZ is not a required one-byte BOOLEAN column");
            check(has_m_column.type == rea_jackcess::DataType::boolean && has_m_column.fixed &&
                      has_m_column.size == 1 && has_m_column.nullable == std::optional<bool>(false),
                  "real sample HasM is not a required one-byte BOOLEAN column");

            std::vector<bool> expected_has_z(18, false);
            expected_has_z[15] = true;
            expected_has_z[16] = true;
            expected_has_z[17] = true;
            check(read_booleans(geometry_columns, has_z, 18) == expected_has_z,
                  "real sample HasZ BOOLEAN bits changed");
            check(read_booleans(geometry_columns, has_m, 18) == std::vector<bool>(18, false),
                  "real sample HasM BOOLEAN bits changed");
        }

        {
            const auto& aces = require_table(tables, "MSysACEs");
            check(aces.row_count_header() == 102, "real sample MSysACEs changed its row count");
            const std::size_t inheritable = column_index(aces, "FInheritable");
            const auto& column = aces.columns()[inheritable];
            check(column.type == rea_jackcess::DataType::boolean && column.fixed && column.size == 1 &&
                      column.nullable == std::optional<bool>(true),
                  "real sample FInheritable is not a nullable one-byte BOOLEAN column");
            const auto values = read_booleans(aces, inheritable, 102);
            check(static_cast<std::size_t>(std::count(values.begin(), values.end(), true)) == 4,
                  "real sample FInheritable BOOLEAN bit count changed");
        }

        std::cout << "real sample BOOLEAN reads passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "real sample BOOLEAN reads failed: " << error.what() << '\n';
        return 1;
    }
}
