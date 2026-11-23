#include "rea_jackcess/database.hpp"

#include "catalog/table_catalog.hpp"
#include "catalog/usage_map.hpp"
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
using rea_jackcess::Table;
using rea_jackcess::TableBuilder;
using rea_jackcess::Value;

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
                ("rea-jackcess-merge-" + std::to_string(stamp));
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
    for (const auto& candidate : database.tables()) {
        if (candidate.is_user_table() && candidate.name() == name) {
            return candidate;
        }
    }
    throw std::runtime_error("missing table " + name);
}

// Counts the pages the index's own usage map still claims, which is how the writer accounts for
// the leaves and node pages it owns.
std::size_t index_page_count(const std::filesystem::path& path) {
    auto file = rea_jackcess::detail::PageFile::open(path);
    const auto catalog = rea_jackcess::detail::read_table_schema_catalog(file);
    for (const auto& table : catalog.tables) {
        if (table.name != "Merged") {
            continue;
        }
        const auto& index = table.definition.physical_indexes.at(0);
        const auto pages = rea_jackcess::detail::read_usage_map_pages(file, index.owned_pages);
        std::size_t count = 0;
        for (const auto page : pages) {
            (void)page;
            ++count;
        }
        return count;
    }
    throw std::runtime_error("Merged table is missing from the catalog");
}

std::vector<std::int32_t> index_order(const Database& database) {
    auto table = find_table(database, "Merged");
    std::vector<std::int32_t> keys;
    for (auto cursor = table.rows_by_index("ByKey"); auto row = cursor.next();) {
        keys.push_back(std::get<std::int32_t>(row->at(0)));
    }
    return keys;
}

void test_merge(DatabaseFormat format, const std::filesystem::path& path) {
    constexpr int total_rows = 1200;
    Database::create(path, format,
                     {TableBuilder("Merged")
                          .add_column("Key", DataType::int32)
                          .add_column("Label", DataType::text)
                          .add_index(rea_jackcess::IndexBuilder("ByKey", "Key"))});

    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        auto table = find_table(database, "Merged");
        std::vector<std::vector<Value>> rows;
        rows.reserve(total_rows);
        for (int key = 1; key <= total_rows; ++key) {
            rows.push_back({std::int32_t{key}, std::string("row")});
        }
        table.add_rows(rows);
        check(table.row_count_header() == total_rows, "the merge fixture did not insert every row");
        check(index_order(database).size() == total_rows, "the index scan lost rows after the inserts");
    }

    const auto pages_after_insert = index_page_count(path);
    check(pages_after_insert >= 4,
          "the fixture needs several index pages to exercise merging, got " +
              std::to_string(pages_after_insert));

    {
        auto database = Database::open(path, DatabaseOpenMode::read_write);
        auto table = find_table(database, "Merged");
        // Keep every fortieth row so the deletions are spread across every leaf. The row ids are
        // collected first: an open index cursor must not outlive a rewrite of the pages it reads.
        std::vector<std::int32_t> keep;
        std::vector<rea_jackcess::RowId> doomed;
        for (auto cursor = table.rows_by_index("ByKey"); auto row = cursor.next();) {
            const auto key = std::get<std::int32_t>(row->at(0));
            if (key % 40 == 0) {
                keep.push_back(key);
                continue;
            }
            doomed.push_back(row->id());
        }
        check(doomed.size() == total_rows - 30,
              "unexpected number of selected rows: " + std::to_string(doomed.size()));
        for (const auto& row_id : doomed) {
            table.delete_row(row_id);
        }
        check(table.row_count_header() == 30, "the table row count is wrong after the deletes");

        // The survivors must stay reachable through the index and in ascending order.
        const auto ordered = index_order(database);
        check(ordered.size() == keep.size(), "the index scan does not return every survivor");
        check(ordered == keep, "the index scan order changed after the deletes");
        for (const auto key : keep) {
            const auto found = table.find_row_by_index("ByKey", std::vector<Value>{std::int32_t{key}});
            check(found.has_value(), "a surviving key is missing from the index: " +
                                         std::to_string(key));
        }
        check(!table.find_row_by_index("ByKey", std::vector<Value>{std::int32_t{41}}).has_value(),
              "a deleted key is still present in the index");
    }

    const auto pages_after_delete = index_page_count(path);
    check(pages_after_delete < pages_after_insert,
          "deleting most rows did not release index pages: " + std::to_string(pages_after_insert) +
              " -> " + std::to_string(pages_after_delete));
    check(pages_after_delete <= 2,
          "the merged index should fit in a root leaf page, still uses " +
              std::to_string(pages_after_delete));

    {
        // A fresh handle proves the merged pages serialize and reparse consistently.
        auto reopened = Database::open(path);
        auto table = find_table(reopened, "Merged");
        check(table.row_count_header() == 30, "the reopened table lost rows");
        const auto ordered = index_order(reopened);
        check(ordered.size() == 30, "the reopened index scan lost rows");
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            check(ordered[index] == static_cast<std::int32_t>((index + 1) * 40),
                  "the reopened index order is wrong at position " + std::to_string(index));
        }
    }
}

int parse_format(const std::string& text) { return text == "v2003" ? 1 : 0; }

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: index_merge_test <v2000|v2003>\n";
        return 2;
    }
    TemporaryDirectory directory;
    const auto format = parse_format(argv[1]) == 1 ? DatabaseFormat::v2003 : DatabaseFormat::v2000;
    try {
        test_merge(format, directory.file("merge.mdb"));
        std::cout << "index merge tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "index merge tests failed: " << error.what() << '\n';
        return 1;
    }
}
