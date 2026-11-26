#include "rea_jackcess/database.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rea_jackcess::Database;
using rea_jackcess::DatabaseError;
using rea_jackcess::OleStream;
using rea_jackcess::Table;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Table find_table(const Database& database, const std::string& name) {
    for (const auto& table : database.tables()) {
        if (table.name() == name) {
            return table;
        }
    }
    throw std::runtime_error("fixture table is missing: " + name);
}

}  // namespace

// The OLE column of LongValues holds a 9000-byte ramp that spans several data pages.
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: ole_stream <jet4-index-v2000.mdb>\n";
        return 2;
    }
    const std::filesystem::path path(argv[1]);
    if (!std::filesystem::exists(path)) {
        std::cout << "fixture is missing, run the generator first: " << path << '\n';
        return 77;
    }
    try {
        auto database = Database::open(path);
        auto table = find_table(database, "LongValues");
        auto cursor = table.rows();
        const auto row = cursor.next();
        check(row.has_value(), "LongValues has no row");

        OleStream stream = table.ole_stream("OleVal", *row);
        check(stream.size() == 9000, "OLE stream size changed");
        check(stream.column_name() == "OleVal", "OLE stream lost its column name");
        check(stream.position() == 0, "OLE stream starts at a non-zero position");

        std::vector<std::uint8_t> head(100);
        check(stream.read(head.data(), head.size()) == head.size(), "short read at the start");
        for (std::size_t index = 0; index < head.size(); ++index) {
            check(head[index] == static_cast<std::uint8_t>((0x40 + index) & 0xffu),
                  "OLE stream head differs at byte " + std::to_string(index));
        }
        check(stream.position() == head.size(), "OLE stream position did not advance");

        stream.seek(4000);
        std::vector<std::uint8_t> middle(8);
        check(stream.read(middle.data(), middle.size()) == middle.size(), "short read in the middle");
        for (std::size_t index = 0; index < middle.size(); ++index) {
            const auto expected = static_cast<std::uint8_t>((0x40 + 4000 + index) & 0xffu);
            check(middle[index] == expected, "OLE stream middle differs");
        }

        const auto tail = stream.read_range(8990, 20);
        check(tail.size() == 10, "read_range did not stop at the end of the stream");
        check(tail.front() == static_cast<std::uint8_t>((0x40 + 8990) & 0xffu), "tail bytes differ");

        const auto all = stream.read_all();
        check(all.size() == 9000, "read_all returned a different size");
        check(all[5000] == static_cast<std::uint8_t>((0x40 + 5000) & 0xffu), "read_all bytes differ");

        // A non-OLE column is rejected instead of returning the decoded text.
        try {
            (void)table.ole_stream("MemoVal", *row);
        } catch (const DatabaseError&) {
            std::cout << "OLE stream checks passed\n";
            return 0;
        }
        throw std::runtime_error("ole_stream accepted a MEMO column");
    } catch (const std::exception& error) {
        std::cerr << "OLE stream check failed: " << error.what() << '\n';
        return 1;
    }
}
