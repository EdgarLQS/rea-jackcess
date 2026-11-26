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

const rea_jackcess::QueryInfo& query_named(const std::vector<rea_jackcess::QueryInfo>& queries,
                                           const std::string& name) {
    const auto found = std::find_if(queries.begin(), queries.end(), [&](const auto& query) {
        return query.name == name;
    });
    check(found != queries.end(), "saved query is missing: " + name);
    return *found;
}

}  // namespace

// Jet3 saved queries. The reconstructed SQL must match the fixed Jackcess 5.2.0 toSQLString
// output byte for byte; the four strings below come from queryV1997.mdb.
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: jet3_query_read <queryV1997.mdb>\n";
        return 2;
    }
    const std::filesystem::path path(argv[1]);
    if (!std::filesystem::exists(path)) {
        std::cout << "Jet3 sample is missing: " << path << '\n';
        return 77;
    }
    try {
        const std::string before = read_bytes(path);
        auto database = Database::open(path);
        const auto& queries = database.queries();
        check(queries.size() == 9, "Jet3 saved query count changed");

        std::vector<std::string> names;
        for (const auto& query : queries) {
            names.push_back(query.name);
        }
        const std::vector<std::string> expected_names = {
            "AppendQuery", "CrosstabQuery", "DataDefinitionQuery", "DeleteQuery", "MakeTableQuery",
            "PassthroughQuery", "SelectQuery", "UnionQuery", "UpdateQuery"};
        check(names == expected_names, "Jet3 saved query list changed");

        check(query_named(queries, "UnionQuery").to_sql() ==
                  "Select Table1.col1, Table1.col2\nwhere Table1.col1 = \"foo\"\nUNION\n"
                  "Select Table2.col1, Table2.col2\nUNION ALL Select Table3.col1, Table3.col2\n"
                  "where Table3.col3 > \"blah\";",
              "Jet3 union query SQL changed");
        check(query_named(queries, "DeleteQuery").to_sql() ==
                  "DELETE Table1.col1, Table1.col2, Table1.col3\nFROM Table1\n"
                  "WHERE (((Table1.col1)>\"blah\"));",
              "Jet3 delete query SQL changed");
        check(query_named(queries, "UpdateQuery").to_sql() ==
                  "PARAMETERS User Name Text;\nUPDATE Table1\n"
                  "SET Table1.col1 = \"foo\", Table1.col2 = [Table2].[col3], "
                  "[Table2].[col1] = [User Name]\nWHERE ((([Table2].[col1]) Is Not Null));",
              "Jet3 update query SQL changed");
        check(query_named(queries, "SelectQuery").to_sql() ==
                  "SELECT DISTINCT Table1.*, Table2.col1, Table2.col2, Table3.col3\n"
                  "FROM (Table1 LEFT JOIN Table3 ON Table1.col1 = Table3.col1) "
                  "INNER JOIN Table2 ON (Table3.col1 = Table2.col2) AND (Table3.col1 = Table2.col1)\n"
                  "WHERE (((Table2.col2)=\"foo\" Or (Table2.col2) In (\"buzz\",\"bazz\")))\n"
                  "ORDER BY Table2.col1;",
              "Jet3 select query SQL changed");
        for (const auto& query : queries) {
            check(!query.to_sql().empty(), "Jet3 saved query produced empty SQL: " + query.name);
        }

        check(read_bytes(path) == before, "reading Jet3 queries modified the sample");
        std::cout << "Jet3 query checks passed: " << path.filename().string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Jet3 query check failed: " << error.what() << '\n';
        return 1;
    }
}
