#include "rea_jackcess/database.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {

rea_jackcess::Table get_table(const rea_jackcess::Database& database, const char* name) {
    const auto tables = database.tables();
    const auto found = std::find_if(tables.begin(), tables.end(), [&](const auto& table) {
        return table.name() == name;
    });
    if (found == tables.end()) {
        throw std::runtime_error("Foreign-key fixture table missing");
    }
    return *found;
}

std::string read_bytes(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Could not read the foreign-key fixture");
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

template <typename Operation>
void expect_rejection(rea_jackcess::DatabaseErrorCode expected, Operation operation) {
    try {
        operation();
    } catch (const rea_jackcess::DatabaseError& error) {
        if (error.code() == expected) {
            return;
        }
        throw;
    }
    throw std::runtime_error("Foreign-key violation was accepted");
}

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: foreign_key_check <writable-copy-of-relationship-fixture.mdb>\n";
        return 2;
    }
    try {
        using rea_jackcess::Value;
        auto db = rea_jackcess::Database::open(argv[1], rea_jackcess::DatabaseOpenMode::read_write);
        auto parent = get_table(db, "Parent");
        auto child = get_table(db, "Child");
        const auto parent_id = parent.add_row({1, 2});
        const auto child_id = child.add_row({1, 2});
        const auto null_id = child.add_row({Value{}, Value{}});
        const auto before = read_bytes(argv[1]);
        expect_rejection(rea_jackcess::DatabaseErrorCode::constraint_violation,
                         [&] { (void)child.add_rows({{1, 2}, {1, 3}}); });
        expect_rejection(rea_jackcess::DatabaseErrorCode::constraint_violation,
                         [&] { (void)child.add_row({1, Value{}}); });
        expect_rejection(rea_jackcess::DatabaseErrorCode::constraint_violation,
                         [&] { child.update_row(null_id, {2, 3}); });
        if (read_bytes(argv[1]) != before) {
            throw std::runtime_error("Foreign-key rejection changed the file");
        }
        // This fixture carries cascade updates and deletes, so the parent-side changes rewrite
        // the dependent rows instead of being rejected.
        parent.update_row(parent_id, {1, 4});
        if (!child.find_row_by_index("ParentChild", std::vector<rea_jackcess::Value>{1, 4})) {
            throw std::runtime_error("Cascade update did not move the child row");
        }
        child.update_row(null_id, {1, 4});
        parent.delete_row(parent_id);
        if (child.row_count_header() != 0) {
            throw std::runtime_error("Cascade delete did not remove the child rows");
        }
        (void)child_id;
        std::cout << "Verified foreign-key preflight, cascades and valid writes\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
