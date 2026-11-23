#include "rea_jackcess/database.hpp"

#include "catalog/table_catalog.hpp"
#include "page/page_file.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rea_jackcess::Database;
using rea_jackcess::DatabaseFormat;
using rea_jackcess::DatabaseOpenMode;
using rea_jackcess::DataType;
using rea_jackcess::IndexBuilder;
using rea_jackcess::Table;
using rea_jackcess::TableBuilder;
using rea_jackcess::Value;

constexpr int column_count = 55;
constexpr int index_count = 30;

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
                ("rea-jackcess-multipage-" + std::to_string(stamp));
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

Table find_table(const Database& database, const std::string& name) {
    for (auto table : database.tables()) {
        if (table.is_user_table() && table.name() == name) {
            return table;
        }
    }
    throw std::runtime_error("missing table " + name);
}

TableBuilder wide_schema() {
    TableBuilder builder("Wide");
    for (int column = 1; column <= column_count; ++column) {
        builder.add_column("ColumnNumber" + std::to_string(column), DataType::int32);
    }
    return builder;
}

std::string index_name(int position) {
    return "WideIndexName" + std::to_string(position) + "WithPaddingToGrowTheDefinition";
}

// Returns how many continuation pages the table definition chain has and its declared size.
std::pair<std::size_t, std::uint32_t> definition_shape(const std::filesystem::path& path) {
    auto file = rea_jackcess::detail::PageFile::open(path);
    const auto catalog = rea_jackcess::detail::read_table_schema_catalog(file);
    for (const auto& table : catalog.tables) {
        if (table.name != "Wide") {
            continue;
        }
        const auto page = file.read_page(table.definition_page_number);
        const auto size = static_cast<std::uint32_t>(page[8]) |
                          (static_cast<std::uint32_t>(page[9]) << 8) |
                          (static_cast<std::uint32_t>(page[10]) << 16) |
                          (static_cast<std::uint32_t>(page[11]) << 24);
        std::uint32_t next = static_cast<std::uint32_t>(page[4]) |
                             (static_cast<std::uint32_t>(page[5]) << 8) |
                             (static_cast<std::uint32_t>(page[6]) << 16) |
                             (static_cast<std::uint32_t>(page[7]) << 24);
        std::size_t continuations = 0;
        while (next != 0) {
            const auto continuation = file.read_page(next);
            check(continuation[0] == 0x02, "a definition continuation page has the wrong page type");
            ++continuations;
            next = static_cast<std::uint32_t>(continuation[4]) |
                   (static_cast<std::uint32_t>(continuation[5]) << 8) |
                   (static_cast<std::uint32_t>(continuation[6]) << 16) |
                   (static_cast<std::uint32_t>(continuation[7]) << 24);
        }
        return {continuations, size};
    }
    throw std::runtime_error("Wide table is missing from the catalog");
}

void test_multipage(DatabaseFormat format, const std::filesystem::path& path) {
    Database::create(path, format, {wide_schema()});
    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        auto table = find_table(database, "Wide");
        std::vector<Value> first;
        std::vector<Value> second;
        for (int column = 1; column <= column_count; ++column) {
            first.push_back(std::int32_t{column});
            second.push_back(std::int32_t{column * 2});
        }
        table.add_rows({first, second});
        check(definition_shape(path).first == 0,
              "the fixture should start with a single-page definition");
    }

    std::size_t continuations_at_crossing = 0;
    std::uint32_t size_at_crossing = 0;
    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        auto table = find_table(database, "Wide");
        for (int position = 1; position <= index_count; ++position) {
            table.add_index(IndexBuilder(index_name(position), "ColumnNumber" + std::to_string(position)));
            const auto [continuations, size] = definition_shape(path);
            if (continuations > 0 && continuations_at_crossing == 0) {
                continuations_at_crossing = continuations;
                size_at_crossing = size;
                check(size > 4096, "a definition that spills a page must exceed the page size");
            }
            check(continuations >= continuations_at_crossing,
                  "adding an index shrank the definition chain");
        }
        check(table.indexes().size() == index_count,
              "the widened table does not report every index");
    }

    check(continuations_at_crossing > 0,
          "the definition never grew past a single page, so multi-page handling was not exercised");
    const auto [final_continuations, final_size] = definition_shape(path);
    check(final_continuations >= continuations_at_crossing,
          "the definition chain lost a page");
    check(final_size > size_at_crossing, "the definition size did not grow with the indexes");

    {
        // A fresh handle parses the repacked chain and every index stays usable.
        auto database = Database::open(path);
        auto table = find_table(database, "Wide");
        check(table.columns().size() == column_count, "the reopened table lost columns");
        check(table.indexes().size() == index_count, "the reopened table lost indexes");
        check(table.indexes().back().name == index_name(index_count),
              "the last index name was not preserved");
        for (int position : {1, index_count / 2, index_count}) {
            const auto hit = table.find_row_by_index(index_name(position),
                                                     std::vector<Value>{std::int32_t{position}});
            check(hit.has_value(), "an index scan cannot find a row through index " +
                                       std::to_string(position));
        }
        const auto miss = table.find_row_by_index(index_name(2), std::vector<Value>{std::int32_t{4242}});
        check(!miss.has_value(), "an index lookup returned a row for an absent key");
    }
}

int parse_format(const std::string& text) { return text == "v2003" ? 1 : 0; }

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: multipage_definition_test <v2000|v2003>\n";
        return 2;
    }
    TemporaryDirectory directory;
    const auto format = parse_format(argv[1]) == 1 ? DatabaseFormat::v2003 : DatabaseFormat::v2000;
    try {
        test_multipage(format, directory.file("wide.mdb"));
        std::cout << "multi-page definition tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "multi-page definition tests failed: " << error.what() << '\n';
        return 1;
    }
}
