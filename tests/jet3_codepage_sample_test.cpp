#include "rea_jackcess/database.hpp"
#include "rea_jackcess/database_file.hpp"

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
using rea_jackcess::DatabaseOptions;
using rea_jackcess::FileFormat;
using rea_jackcess::MemoValue;
using rea_jackcess::Row;
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

std::vector<Row> read_all(rea_jackcess::RowCursor cursor) {
    std::vector<Row> rows;
    while (auto row = cursor.next()) {
        rows.push_back(*row);
    }
    return rows;
}

// Access 97 database created with DAO 3.6 in the Japanese collating order (code page 932). See
// testdata/SOURCES.md in dominion525/jetdb for the generator and the expected contents.
void check_japanese_932(const std::filesystem::path& path) {
    const std::string before = read_bytes(path);
    DatabaseOptions options;
    options.code_page = 932;
    auto database = Database::open(path, options);
    check(database.file_format() == FileFormat::v1997, "932 sample is not Jet3");

    Table customers = find_table(database, "顧客");
    check(customers.columns().size() == 3, "顧客 column count changed");
    check(customers.columns()[0].name == "ID", "顧客 ID column name changed");
    check(customers.columns()[1].name == "氏名", "顧客 氏名 column name changed");
    check(customers.columns()[2].name == "備考", "顧客 備考 column name changed");

    const auto rows = read_all(customers.rows());
    check(rows.size() == 3, "顧客 row count changed");
    check(std::get<std::int32_t>(rows[0].at(0)) == 1 &&
              std::get<std::string>(rows[0].at(1)) == "山田太郎",
          "顧客 first row changed");
    const auto* memo = std::get_if<MemoValue>(&rows[0].at(2));
    check(memo != nullptr && memo->text == "東京都千代田区", "顧客 first memo changed");
    check(std::get<std::int32_t>(rows[1].at(0)) == 2 &&
              std::get<std::string>(rows[1].at(1)) == "ﾃｽﾄ",
          "顧客 second row changed");
    const auto* second_memo = std::get_if<MemoValue>(&rows[1].at(2));
    check(second_memo != nullptr && second_memo->text == "①～髙", "顧客 second memo changed");
    check(std::get<std::string>(rows[1].at(1)) != std::get<std::string>(rows[2].at(1)),
          "932 row values did not decode independently");

    check(customers.indexes().size() == 1, "顧客 index count changed");
    check(customers.indexes()[0].name == "氏名索引", "顧客 index name changed");
    std::vector<std::string> indexed_names;
    for (const auto& row : read_all(customers.rows_by_index("氏名索引"))) {
        indexed_names.push_back(std::get<std::string>(row.at(1)));
    }
    check(indexed_names == std::vector<std::string>({"Smith", "ﾃｽﾄ", "山田太郎"}),
          "顧客 index scan order changed");

    check(read_bytes(path) == before, "reading the 932 sample modified it");
}

// Access 97 database whose text cell uses GBK (code page 936). The expected value is asserted by
// UCanAccess's CharsetGBKTest.
void check_gbk_936(const std::filesystem::path& path) {
    const std::string before = read_bytes(path);
    DatabaseOptions options;
    options.code_page = 936;
    auto database = Database::open(path, options);
    check(database.file_format() == FileFormat::v1997, "936 sample is not Jet3");

    Table table = find_table(database, "table1");
    check(table.columns().size() == 2, "table1 column count changed");
    check(table.columns()[1].name == "field1", "table1 field1 name changed");
    const auto rows = read_all(table.rows());
    check(rows.size() == 1, "table1 row count changed");
    check(std::get<std::int32_t>(rows[0].at(0)) == 2 &&
              std::get<std::string>(rows[0].at(1)) == "GBK编码",
          "table1 GBK value changed");

    check(table.indexes().size() == 1 && table.indexes()[0].name == "PrimaryKey",
          "table1 index changed");
    const auto found = table.find_row_by_index("PrimaryKey", std::int32_t{2});
    check(found.has_value() && std::get<std::string>(found->at(1)) == "GBK编码",
          "table1 primary key lookup changed");

    check(read_bytes(path) == before, "reading the 936 sample modified it");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: jet3_codepage_sample <932|936> <jet3-sample.mdb>\n";
        return 2;
    }
    const std::string mode(argv[1]);
    const std::filesystem::path path(argv[2]);
    if (!std::filesystem::exists(path)) {
        std::cout << "Jet3 code page sample is missing: " << path << '\n';
        return 77;
    }
    try {
        if (mode == "932") {
            check_japanese_932(path);
        } else if (mode == "936") {
            check_gbk_936(path);
        } else {
            std::cerr << "unknown mode: " << mode << '\n';
            return 2;
        }
        std::cout << "Jet3 code page checks passed: " << path.filename().string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Jet3 code page check failed: " << error.what() << '\n';
        return 1;
    }
}
