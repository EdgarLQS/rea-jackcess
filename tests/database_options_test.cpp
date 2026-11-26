#include "rea_jackcess/database.hpp"

#include "util/code_page.hpp"

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
using rea_jackcess::DatabaseErrorCode;
using rea_jackcess::DatabaseOptions;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "could not read the Jet3 sample");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: database_options <common1V1997.mdb>\n";
        return 2;
    }
    const std::filesystem::path path(argv[1]);
    if (!std::filesystem::exists(path)) {
        std::cout << "Jet3 sample is missing: " << path << '\n';
        return 77;
    }
    try {
        // Windows-1252 maps 0x80 to the euro sign; Latin-1 keeps it as a control character.
        check(rea_jackcess::detail::decode_code_page_text({0x80}, 1252) == "\xe2\x82\xac",
              "1252 decoding of 0x80 changed");
        check(rea_jackcess::detail::decode_code_page_text({0x80}, 819) == "\xc2\x80",
              "819 decoding of 0x80 changed");
        try {
            (void)rea_jackcess::detail::decode_code_page_text({0x80}, 936);
        } catch (const std::invalid_argument&) {
            // expected
        }

        // A supported override opens normally; Jet3 names are ASCII so they read unchanged.
        DatabaseOptions latin1;
        latin1.code_page = 819;
        auto database = Database::open(path, latin1);
        check(database.tables().size() == 10, "code-page override changed the table list");
        for (const auto& table : database.tables()) {
            if (table.name() != "Table1") {
                continue;
            }
            auto cursor = table.rows();
            const auto row = cursor.next();
            check(row.has_value() && std::get<std::string>(row->at(0)) == "a",
                  "code-page override changed Table1 row 1");
        }

        // The same override works for a memory buffer.
        auto from_memory = Database::open(read_file(path), latin1);
        check(from_memory.tables().size() == 10, "memory open ignored the code-page override");

        // Anything outside the supported set is rejected when opening.
        DatabaseOptions unsupported;
        unsupported.code_page = 936;
        try {
            (void)Database::open(path, unsupported);
        } catch (const DatabaseError& error) {
            check(error.code() == DatabaseErrorCode::unsupported_operation,
                  "unsupported code page used the wrong error code");
            std::cout << "database options checks passed\n";
            return 0;
        }
        throw std::runtime_error("an unsupported code page was accepted");
    } catch (const std::exception& error) {
        std::cerr << "database options check failed: " << error.what() << '\n';
        return 1;
    }
}
