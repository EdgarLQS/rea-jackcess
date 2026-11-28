#include "rea_jackcess/database.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <vector>

// Exercises the public read path with arbitrary file bytes: open the in-memory database, enumerate
// its tables and read a bounded prefix of each table's rows. Malformed input is expected to throw;
// the fuzzer only looks for crashes, hangs and sanitizer findings.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    constexpr std::size_t minimum_file_size = 4096;
    constexpr std::size_t maximum_file_size = 64u * 1024u * 1024u;
    constexpr std::size_t maximum_tables = 10000;
    constexpr std::size_t maximum_rows_per_table = 256;
    if (size < minimum_file_size || size > maximum_file_size) {
        return 0;
    }

    try {
        const std::vector<std::uint8_t> bytes(data, data + size);
        rea_jackcess::Database database = rea_jackcess::Database::open(bytes);

        std::size_t table_count = 0;
        for (const auto& table : database.tables()) {
            if (++table_count > maximum_tables) {
                break;
            }
            auto cursor = table.rows();
            std::size_t row_count = 0;
            while (cursor.next()) {
                if (++row_count >= maximum_rows_per_table) {
                    break;
                }
            }
        }
    } catch (const std::exception&) {
        // Malformed input is a normal outcome for a parser.
    } catch (...) {
        // Some third-party or platform errors may not derive from std::exception.
    }
    return 0;
}
