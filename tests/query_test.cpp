#include "rea_jackcess/database.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rea_jackcess::Database;
using rea_jackcess::DatabaseOpenMode;
using rea_jackcess::QueryInfo;
using rea_jackcess::QueryType;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("rea-jackcess-query-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct ExpectedQuery {
    std::string name;
    QueryType type;
    std::int32_t object_id;
    std::uint32_t object_flags;
    std::string sql;
};

// Expected SQL is byte-for-byte what the fixed Jackcess 5.2.0 JAR reports through
// Query.toSQLString() for the committed fixtures.
std::vector<ExpectedQuery> expected_queries() {
    const std::string select_sql = R"SQL(SELECT DISTINCT Table1.*, Table2.col1, Table2.col2, Table3.col3
FROM (Table1 LEFT JOIN Table3 ON Table1.col1 = Table3.col1) INNER JOIN Table2 ON (Table3.col1 = Table2.col2) AND (Table3.col1 = Table2.col1)
WHERE (((Table2.col2)="foo" Or (Table2.col2) In ("buzz","bazz")))
ORDER BY Table2.col1;)SQL";
    const std::string update_sql = R"SQL(PARAMETERS User Name Text;
UPDATE Table1
SET Table1.col1 = "foo", Table1.col2 = [Table2].[col3], [Table2].[col1] = [User Name]
WHERE ((([Table2].[col1]) Is Not Null));)SQL";
    const std::string append_sql = R"SQL(INSERT INTO Table3 (col2, col2, col3)
SELECT [Table1].[col2], [Table2].[col2], [Table2].[col3]
FROM Table3, Table1 INNER JOIN Table2 ON [Table1].[col1]=[Table2].[col1];)SQL";
    const std::string delete_sql = R"SQL(DELETE Table1.col1, Table1.col2, Table1.col3
FROM Table1
WHERE (((Table1.col1)>"blah"));)SQL";
    const std::string make_table_sql = R"SQL(SELECT Max(Table2.col1) AS MaxOfcol1, Table2.col2, Table3.col2 INTO Table4
FROM (Table2 INNER JOIN Table1 ON Table2.col1 = Table1.col2) RIGHT JOIN Table3 ON Table1.col2 = Table3.col3
GROUP BY Table2.col2, Table3.col2
HAVING (((Max(Table2.col1))="buzz") AND ((Table2.col2)<>"blah"));)SQL";
    const std::string crosstab_sql = R"SQL(TRANSFORM Count([Table2].[col2]) AS CountOfcol2
SELECT Table2_1.col1, [Table2].[col3], Avg(Table2_1.col2) AS AvgOfcol2
FROM (Table1 INNER JOIN Table2 ON [Table1].[col1]=[Table2].[col1]) INNER JOIN Table2 AS Table2_1 ON [Table2].[col1]=Table2_1.col3
WHERE ((([Table1].[col1])>"10") And ((Table2_1.col1) Is Not Null) And ((Avg(Table2_1.col2))>"10"))
GROUP BY Table2_1.col1, [Table2].[col3]
ORDER BY [Table2].[col3]
PIVOT [Table1].[col1];)SQL";
    const std::string union_sql = R"SQL(Select Table1.col1, Table1.col2
where Table1.col1 = "foo"
UNION
Select Table2.col1, Table2.col2
UNION ALL Select Table3.col1, Table3.col2
where Table3.col3 > "blah";)SQL";

    // The stored passthrough and DDL expressions keep their trailing NUL, and the fixed JAR
    // prints it as part of the statement, so the expectation carries the same byte.
    const std::string passthrough_sql("ALTER TABLE Table4 DROP COLUMN col5;\0",
                                      sizeof("ALTER TABLE Table4 DROP COLUMN col5;") );
    const std::string ddl_sql("CREATE TABLE Table5 (col1 CHAR, col2 CHAR);\0",
                              sizeof("CREATE TABLE Table5 (col1 CHAR, col2 CHAR);"));

    return {
        {"SelectQuery", QueryType::select, -2147483640, 0, select_sql},
        {"UpdateQuery", QueryType::update, -2147483639, 48, update_sql},
        {"AppendQuery", QueryType::append, -2147483638, 64, append_sql},
        {"DeleteQuery", QueryType::delete_rows, -2147483636, 32, delete_sql},
        {"MakeTableQuery", QueryType::make_table, -2147483635, 80, make_table_sql},
        {"CrosstabQuery", QueryType::cross_tab, -2147483634, 16, crosstab_sql},
        {"UnionQuery", QueryType::union_query, -2147483633, 128, union_sql},
        {"PassthroughQuery", QueryType::passthrough, -2147483632, 112, passthrough_sql},
        {"DataDefinitionQuery", QueryType::data_definition, -2147483631, 96, ddl_sql},
    };
}

void test_saved_queries(const std::filesystem::path& fixture) {
    const auto database = Database::open(fixture, DatabaseOpenMode::read_only);
    const auto queries = database.queries();
    const auto expected = expected_queries();
    check(queries.size() == expected.size(),
          "expected " + std::to_string(expected.size()) + " saved queries, found " +
              std::to_string(queries.size()));

    for (const auto& item : expected) {
        const auto found = std::find_if(queries.begin(), queries.end(), [&](const auto& query) {
            return query.name == item.name;
        });
        check(found != queries.end(), "saved query is missing: " + item.name);
        check(found->type == item.type, item.name + ": unexpected query type");
        check(found->object_id == item.object_id, item.name + ": unexpected object id");
        check(found->object_flags == item.object_flags, item.name + ": unexpected object flags");
        const auto sql = found->to_sql();
        check(sql == item.sql,
              item.name + ": SQL differs from the fixed Jackcess 5.2.0 output\n--- expected ---\n" +
                  item.sql + "\n--- actual ---\n" + sql);
    }

    // Reading the same file twice must stay stable: the rows are rebuilt from the catalog.
    const auto again = Database::open(fixture, DatabaseOpenMode::read_only);
    check(again.queries().size() == expected.size(), "a second read returned a different query count");
}

void test_query_errors(const std::filesystem::path& directory) {
    const auto empty_path = directory / "queries-empty.mdb";
    Database::create(empty_path, rea_jackcess::DatabaseFormat::v2000);
    check(Database::open(empty_path).queries().empty(),
          "a database without saved queries should report none");

    QueryInfo unknown;
    unknown.type = QueryType::unknown;
    bool rejected = false;
    try {
        (void)unknown.to_sql();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "an unknown query type must not produce SQL");

    QueryInfo ddl;
    ddl.type = QueryType::data_definition;
    check(ddl.to_sql().empty(), "a data definition query without an expression should render empty");
}

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: query_test <saved-query-fixture.mdb>\n";
        return 2;
    }
    try {
        TemporaryDirectory directory;
        test_saved_queries(argv[1]);
        test_query_errors(directory.path());
        std::cout << "saved query tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "saved query tests failed: " << error.what() << '\n';
        return 1;
    }
}
