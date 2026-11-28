#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rea_jackcess {

// The kind of a saved Access query, mapped from its stored query flags.
enum class QueryType {
    unknown,          // The stored flags do not match a recognised query kind.
    select,           // SELECT query.
    make_table,       // SELECT ... INTO / make-table (action) query.
    append,           // INSERT INTO ... SELECT append query.
    update,           // UPDATE query.
    delete_rows,      // DELETE query.
    cross_tab,        // TRANSFORM ... PIVOT crosstab query.
    data_definition,  // DDL query (CREATE/DROP ...).
    passthrough,      // SQL passthrough query executed by an external source.
    union_query,      // UNION query.
};

// One stored row of a query definition, before it is reconstructed into SQL text. Each row
// describes one part of the query; which fields carry meaning depends on `attribute`, so all are
// optional.
struct QueryRowInfo {
    // Row-kind selector. Known values: 1 = query type / target table, 4 = remote (IN 'path')
    // clause, 5 = FROM source, 6 = SELECT/column expression, 7 = join, 8 = WHERE, 9 = GROUP BY,
    // 10 = HAVING. (Observed from the fixed 5.2.0 saved-query reader.)
    std::optional<std::uint8_t> attribute;
    // SQL expression text (a column expression, join/WHERE condition, remote type, etc.).
    std::optional<std::string> expression;
    // Row flag word; its bits refine the row (for example SELECT DISTINCT/TOP bits on the type
    // row, a join type 1..3 on a join row, or a column-visibility bit on a column row).
    std::optional<std::int16_t> flag;
    // Extra integer field whose meaning depends on `attribute` (unused by the SQL reconstruction).
    std::optional<std::int32_t> extra;
    // First name field (table name, column alias, join endpoint, etc.).
    std::optional<std::string> name1;
    // Second name field (column alias, other join endpoint, etc.).
    std::optional<std::string> name2;
    // Raw ordering bytes as stored.
    std::optional<std::vector<std::uint8_t>> order;
};

// Metadata of one saved query (a QueryDef object) read from the database container.
struct QueryInfo {
    // Query object name.
    std::string name;
    // MSysObjects object id.
    std::int32_t object_id;
    // MSysObjects flags word; its high nibble (0xf0) selects the query type when the stored type
    // row is absent, and must agree with the stored type row when both are present.
    std::uint32_t object_flags;
    // Classified query kind.
    QueryType type;
    // The stored definition rows in their on-disk order.
    std::vector<QueryRowInfo> rows;

    // Returns the stored or reconstructed SQL for supported query types.
    // Unsupported types and malformed definitions raise std::invalid_argument.
    std::string to_sql() const;
};

}
