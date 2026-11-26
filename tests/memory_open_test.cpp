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
using rea_jackcess::DatabaseError;
using rea_jackcess::FileFormat;
using rea_jackcess::Table;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "could not read " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<std::string> table_names(const Database& database) {
    std::vector<std::string> names;
    for (const auto& table : database.tables()) {
        names.push_back(table.name());
    }
    return names;
}

std::vector<std::int32_t> first_column(const Table& table) {
    std::vector<std::int32_t> values;
    auto cursor = table.rows();
    while (auto row = cursor.next()) {
        if (const auto* number = std::get_if<std::int32_t>(&row->at(0))) {
            values.push_back(*number);
        }
    }
    return values;
}

const Table& find_table(const std::vector<Table>& tables, const std::string& name) {
    const auto found = std::find_if(tables.begin(), tables.end(), [&](const auto& table) {
        return table.name() == name;
    });
    check(found != tables.end(), "table is missing: " + name);
    return *found;
}

// The same bytes must produce the same catalog and rows whether they come from a file or a buffer.
void check_matches_file(const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    const auto original = bytes;

    auto from_file = Database::open(path);
    auto from_memory = Database::open(bytes);

    check(from_memory.file_format() == from_file.file_format(), "file format differs in memory");
    check(from_memory.path() == std::filesystem::path("<memory>"),
          "memory handle does not report <memory> as its path");
    check(table_names(from_memory) == table_names(from_file), "table list differs in memory");

    const auto file_tables = from_file.tables();
    const auto memory_tables = from_memory.tables();
    for (const auto& table : file_tables) {
        const auto& mirrored = find_table(memory_tables, table.name());
        check(mirrored.row_count_header() == table.row_count_header(),
              "row count differs in memory for " + table.name());
        check(mirrored.columns().size() == table.columns().size(),
              "column count differs in memory for " + table.name());
        check(first_column(mirrored) == first_column(table),
              "first column differs in memory for " + table.name());
    }
    check(bytes == original, "reading an in-memory database modified the caller's buffer");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: memory_open <ace-fixture> <jet4-fixture>\n";
        return 2;
    }
    try {
        check_matches_file(argv[1]);
        check_matches_file(argv[2]);

        // A buffer that does not hold a complete page-aligned file is rejected by the same rules
        // as a truncated file, reporting the memory name.
        auto broken = read_file(argv[1]);
        broken.resize(broken.size() - 4096);
        try {
            (void)Database::open(broken);
        } catch (const DatabaseError& error) {
            check(error.path() == std::filesystem::path("<memory>"),
                  "memory error lost the <memory> path");
            check(error.code() == rea_jackcess::DatabaseErrorCode::corrupted_file ||
                      error.code() == rea_jackcess::DatabaseErrorCode::truncated_file,
                  "memory error used an unexpected code");
        }

        // Memory handles are read-only.
        auto database = Database::open(read_file(argv[1]));
        auto tables = database.tables();
        Table table = tables.front();
        try {
            table.add_rows({{std::int32_t{1}}});
        } catch (const DatabaseError& error) {
            check(std::string(error.what()).find("read-only") != std::string::npos ||
                      error.code() == rea_jackcess::DatabaseErrorCode::unsupported_operation,
                  "unexpected write error for a memory database");
            std::cout << "memory open checks passed\n";
            return 0;
        }
        throw std::runtime_error("a write to a memory database was accepted");
    } catch (const std::exception& error) {
        std::cerr << "memory open check failed: " << error.what() << '\n';
        return 1;
    }
}
