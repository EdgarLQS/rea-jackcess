#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rea_jackcess {

enum class QueryType {
    unknown, select, make_table, append, update, delete_rows, cross_tab,
    data_definition, passthrough, union_query,
};

struct QueryRowInfo {
    std::optional<std::uint8_t> attribute;
    std::optional<std::string> expression;
    std::optional<std::int16_t> flag;
    std::optional<std::int32_t> extra;
    std::optional<std::string> name1;
    std::optional<std::string> name2;
    std::optional<std::vector<std::uint8_t>> order;
};

struct QueryInfo {
    std::string name;
    std::int32_t object_id;
    std::uint32_t object_flags;
    QueryType type;
    std::vector<QueryRowInfo> rows;

    // Returns the stored or reconstructed SQL for supported query types.
    // Unsupported types and malformed definitions raise std::invalid_argument.
    std::string to_sql() const;
};

}
