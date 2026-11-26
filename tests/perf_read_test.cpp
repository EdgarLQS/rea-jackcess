#include "rea_jackcess/database.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/resource.h>

namespace {

using rea_jackcess::Database;
using rea_jackcess::DatabaseFormat;
using rea_jackcess::DecimalValue;
using rea_jackcess::Table;
using rea_jackcess::TableBuilder;
using rea_jackcess::Value;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

long peak_rss_kib() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1;
    }
#ifdef __APPLE__
    // macOS reports bytes, Linux reports kilobytes.
    return usage.ru_maxrss / 1024;
#else
    return usage.ru_maxrss;
#endif
}

std::string code_for(int index) {
    return "code-" + std::to_string(index % 1000);
}

Table find_user_table(const Database& database) {
    for (const auto& table : database.tables()) {
        if (table.is_user_table()) {
            return table;
        }
    }
    throw std::runtime_error("the performance table is missing");
}

}  // namespace

// Builds a Jet4 table with a primary key and a few variable columns, then reads it back. The
// numbers are recorded as machine-local evidence; the acceptance thresholds are not frozen yet,
// so the test only asserts that every row comes back.
int main(int argc, char** argv) {
    const int row_count = argc > 1 ? std::atoi(argv[1]) : 200000;
    // A non-zero memo length turns the run into the large-file scenario: 250k rows with 4000
    // characters each produce roughly one gigabyte.
    const int memo_chars = argc > 2 ? std::atoi(argv[2]) : 0;
    if (row_count <= 0 || memo_chars < 0) {
        std::cerr << "usage: perf_read [row-count] [memo-chars]\n";
        return 2;
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
                           ("rea-jackcess-perf-" + std::to_string(stamp));
    std::filesystem::create_directories(directory);
    const auto path = directory / "perf.mdb";
    try {
        const auto write_start = std::chrono::steady_clock::now();
        {
            TableBuilder rows("Rows");
            rows.add_column("Id", rea_jackcess::DataType::int32);
            rows.add_column("Code", rea_jackcess::DataType::text);
            rows.add_column("Amount", rea_jackcess::DataType::money);
            rows.add_column("Note", rea_jackcess::DataType::text);
            rows.add_column("Body", rea_jackcess::DataType::memo);
            rows.add_index(rea_jackcess::IndexBuilder("PrimaryKey", "Id").with_primary_key());
            Database::create(path, DatabaseFormat::v2000, {rows});
            Database database = Database::open(path, rea_jackcess::DatabaseOpenMode::read_write);
            Table table = find_user_table(database);
            constexpr int batch_size = 5000;
            std::vector<std::vector<Value>> batch;
            batch.reserve(batch_size);
            for (int row = 0; row < row_count; ++row) {
                Value body = std::monostate{};
                if (memo_chars > 0) {
                    const std::string text = "body-" + std::to_string(row) +
                                             std::string(static_cast<std::size_t>(memo_chars), 'm');
                    body = rea_jackcess::MemoValue{text, std::vector<std::uint8_t>(text.begin(), text.end())};
                }
                batch.push_back({row, code_for(row), DecimalValue{"12345.6789"},
                                 "note-" + std::to_string(row), body});
                if (static_cast<int>(batch.size()) == batch_size) {
                    table.add_rows(batch);
                    batch.clear();
                }
            }
            if (!batch.empty()) {
                table.add_rows(batch);
            }
        }
        const auto write_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - write_start)
                                  .count();

        const auto read_start = std::chrono::steady_clock::now();
        std::int64_t id_sum = 0;
        std::int64_t counted = 0;
        {
            Database database = Database::open(path);
            auto cursor = find_user_table(database).rows();
            while (auto row = cursor.next()) {
                const auto* id = std::get_if<std::int32_t>(&row->at(0));
                check(id != nullptr, "Id did not decode");
                id_sum += *id;
                ++counted;
            }
        }
        const auto read_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - read_start)
                                 .count();
        const auto expected_sum = static_cast<std::int64_t>(row_count - 1) * row_count / 2;
        check(counted == row_count, "row count mismatch: " + std::to_string(counted));
        check(id_sum == expected_sum, "Id sum mismatch");

        const auto size = std::filesystem::file_size(path);
        std::cout << "perf_read rows=" << row_count << " memo_chars=" << memo_chars << " bytes=" << size << " write_ms=" << write_ms
                  << " read_ms=" << read_ms << " peak_rss_kib=" << peak_rss_kib() << '\n';
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        std::cerr << "perf_read failed: " << error.what() << '\n';
        return 1;
    }
}
