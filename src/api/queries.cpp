#include "database_internal.hpp"

#include "catalog/system_catalog.hpp"
#include "value/jet4_value.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace rea_jackcess {
namespace {

QueryType classify_query(const QueryInfo& query) {
    constexpr std::array<QueryType, 10> types{
        QueryType::unknown, QueryType::select, QueryType::make_table, QueryType::append,
        QueryType::update, QueryType::delete_rows, QueryType::cross_tab,
        QueryType::data_definition, QueryType::passthrough, QueryType::union_query,
    };
    constexpr std::array<int, types.size()> flags{-1, 0, 80, 64, 48, 32, 16, 96, 112, 128};
    const auto type_row = std::find_if(query.rows.begin(), query.rows.end(), [](const auto& row) {
        return row.attribute == 1;
    });
    const auto recorded_type = type_row == query.rows.end() ? std::nullopt : type_row->flag;
    const auto object_flag = static_cast<int>(query.object_flags & 0xf0);
    if (object_flag == 0 && recorded_type) {
        return *recorded_type > 0 && static_cast<std::size_t>(*recorded_type) < types.size()
            ? types[*recorded_type] : QueryType::unknown;
    }
    const auto found = std::find(flags.begin(), flags.end(), object_flag);
    if (found == flags.end() || (recorded_type && *recorded_type != found - flags.begin())) {
        return QueryType::unknown;
    }
    return types[found - flags.begin()];
}

const QueryRowInfo* unique_query_row(const QueryInfo& query, std::uint8_t attribute) {
    const QueryRowInfo* result = nullptr;
    for (const auto& row : query.rows) {
        if (row.attribute == attribute) {
            if (result) {
                throw std::invalid_argument("Duplicate saved query attribute");
            }
            result = &row;
        }
    }
    return result;
}

std::string clean_union_sql(std::string_view expression) {
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    const auto begin = expression.find_first_not_of(whitespace);
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = expression.find_last_not_of(whitespace);
    std::string result;
    for (auto i = begin; i <= end; ++i) {
        const auto c = expression[i];
        if (c == '\r' || c == '\n') {
            if (i == begin || (expression[i - 1] != '\r' && expression[i - 1] != '\n')) {
                result.push_back('\n');
            }
        } else {
            result.push_back(c);
        }
    }
    return result;
}

std::string quoted_identifier(std::string_view name, bool qualified = false) {
    if (qualified) {
        std::string result;
        auto dot = name.find('.');
        while (dot != std::string_view::npos) {
            result += quoted_identifier(name.substr(0, dot));
            result += '.';
            name.remove_prefix(dot + 1);
            dot = name.find('.');
        }
        result += quoted_identifier(name);
        return result;
    }
    if ((name.size() >= 2 && name.front() == '[' && name.back() == ']') ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") ==
            std::string_view::npos) {
        return std::string(name);
    }
    return "[" + std::string(name) + "]";
}

std::string select_sources(const QueryInfo& query) {
    struct Source {
        std::vector<std::string> names;
        std::string sql;
        std::vector<std::string> conditions;
        std::int16_t join_type = 0;

        std::string render(bool nested) const {
            auto result = sql;
            for (auto it = conditions.rbegin(); it != conditions.rend(); ++it) {
                result += it == conditions.rbegin() ? " ON " : " AND ";
                result += conditions.size() > 1 ? "(" + *it + ")" : *it;
            }
            return nested && join_type != 0 ? "(" + result + ")" : result;
        }
    };
    std::vector<Source> sources;
    for (const auto& row : query.rows) {
        if (row.attribute != 5) {
            continue;
        }
        if (!row.name1) {
            throw std::invalid_argument("Saved SELECT source has no name");
        }
        std::string sql;
        if (row.expression) {
            const auto& prefix = *row.expression;
            sql = !prefix.empty() && prefix.front() == '[' && prefix.back() == ']'
                ? prefix + "." : "[" + prefix + "].";
        }
        sql += quoted_identifier(*row.name1, true);
        if (row.name2) {
            sql += " AS " + quoted_identifier(*row.name2);
        }
        sources.push_back({{row.name2.value_or(*row.name1)}, std::move(sql), {}, 0});
    }
    for (const auto& row : query.rows) {
        if (row.attribute != 7) {
            continue;
        }
        if (!row.name1 || !row.name2 || !row.flag || *row.flag < 1 || *row.flag > 3 || !row.expression) {
            throw std::invalid_argument("Incomplete or unsupported saved SELECT join");
        }
        const auto find_source = [&](const std::string& name) {
            return std::find_if(sources.begin(), sources.end(), [&](const auto& source) {
                return std::find(source.names.begin(), source.names.end(), name) != source.names.end();
            });
        };
        const auto left = find_source(*row.name1);
        const auto right = find_source(*row.name2);
        if (left != sources.end() && left == right) {
            if (left->join_type != *row.flag) {
                throw std::invalid_argument("Saved SELECT join types conflict");
            }
            left->conditions.push_back(*row.expression);
            continue;
        }
        constexpr std::string_view joins[]{"", " INNER JOIN ", " LEFT JOIN ", " RIGHT JOIN "};
        Source combined{{}, left == sources.end() ? quoted_identifier(*row.name1) : left->render(true),
                        {*row.expression}, *row.flag};
        combined.sql += joins[*row.flag];
        combined.sql += right == sources.end() ? quoted_identifier(*row.name2) : right->render(true);
        for (const auto& endpoint : {*row.name1, *row.name2}) {
            const auto source = find_source(endpoint);
            if (source == sources.end()) {
                combined.names.push_back(endpoint);
            } else {
                combined.names.insert(combined.names.end(), source->names.begin(), source->names.end());
                sources.erase(source);
            }
        }
        sources.push_back(std::move(combined));
    }
    std::string sql;
    for (const auto& source : sources) {
        if (!sql.empty()) {
            sql += ", ";
        }
        sql += source.render(false);
    }
    return sql;
}

void append_remote(std::string& sql, const std::optional<std::string>& path,
                   const std::optional<std::string>& type) {
    if (path || type) {
        sql += " IN '" + path.value_or("") + "'";
        if (type) {
            sql += " [" + *type + "]";
        }
    }
}

void append_where(std::string& sql, const QueryInfo& query) {
    if (const auto* where = unique_query_row(query, 8); where && where->expression) {
        sql += "\nWHERE " + *where->expression;
    }
}

const QueryRowInfo& query_target(const QueryInfo& query) {
    const auto* target = unique_query_row(query, 1);
    if (!target || !target->name1) {
        throw std::invalid_argument("Saved query target is missing");
    }
    return *target;
}

void append_select(std::string& sql, const QueryInfo& query, const QueryRowInfo* flag_row) {
    const auto flags = flag_row ? flag_row->flag.value_or(0) : 0;
    sql += query.type == QueryType::delete_rows ? "DELETE " : "SELECT ";
    if (query.type != QueryType::delete_rows && (flags & 0x02)) {
        sql += "DISTINCT ";
    } else if (query.type != QueryType::delete_rows && (flags & 0x08)) {
        sql += "DISTINCTROW ";
    } else if (query.type != QueryType::delete_rows && (flags & 0x10)) {
        if (!flag_row->name1) {
            throw std::invalid_argument("Saved SELECT TOP is missing its count");
        }
        sql += "TOP " + *flag_row->name1 + ((flags & 0x20) ? " PERCENT " : " ");
    }
    const auto append_expressions = [&](std::uint8_t attribute, std::string_view prefix) {
        bool first = true;
        for (const auto& row : query.rows) {
            if (row.attribute != attribute ||
                (query.type == QueryType::cross_tab && (row.flag.value_or(0) & 2) == 0) ||
                (query.type == QueryType::append && attribute == 6 && (row.flag.value_or(0) & 0x8000))) {
                continue;
            }
            if (!row.expression) {
                throw std::invalid_argument("Saved SELECT is missing an expression");
            }
            sql += first ? prefix : ", ";
            sql += *row.expression;
            if (attribute == 6 && row.name1) {
                sql += " AS " + quoted_identifier(*row.name1);
            }
            first = false;
        }
        return !first;
    };
    const bool has_columns = append_expressions(6, "");
    if (flags & 1) {
        sql += has_columns ? ", *" : "*";
    }
    if (query.type == QueryType::make_table) {
        const auto& target = query_target(query);
        sql += " INTO " + quoted_identifier(*target.name1, true);
        append_remote(sql, target.name2, target.expression);
    }
    const auto sources = select_sources(query);
    if (!sources.empty()) {
        sql += "\nFROM " + sources;
        if (const auto* remote = unique_query_row(query, 4)) {
            append_remote(sql, remote->name1, remote->expression);
        }
    }
    append_where(sql, query);
    append_expressions(9, "\nGROUP BY ");
    if (const auto* having = unique_query_row(query, 10); having && having->expression) {
        sql += "\nHAVING " + *having->expression;
    }
}

bool append_insert(std::string& sql, const QueryInfo& query) {
    const auto& target = query_target(query);
    std::string columns;
    std::string values;
    for (const auto& row : query.rows) {
        if (row.attribute != 6) {
            continue;
        }
        if (row.name2) {
            if (!columns.empty()) {
                columns += ", ";
            }
            columns += quoted_identifier(*row.name2, true);
        }
        if ((row.flag.value_or(0) & 0x8000) != 0) {
            if (!row.expression) {
                throw std::invalid_argument("Saved INSERT value is missing");
            }
            if (!values.empty()) {
                values += ", ";
            }
            values += *row.expression;
        }
    }
    sql += "INSERT INTO " + quoted_identifier(*target.name1, true);
    if (!columns.empty()) {
        sql += " (" + columns + ")";
    }
    append_remote(sql, target.name2, target.expression);
    sql += '\n';
    if (!values.empty()) {
        sql += "VALUES (" + values + ")";
        return true;
    }
    return false;
}

void append_update(std::string& sql, const QueryInfo& query) {
    sql += "UPDATE " + select_sources(query);
    if (const auto* remote = unique_query_row(query, 4)) {
        append_remote(sql, remote->name1, remote->expression);
    }
    bool first = true;
    for (const auto& row : query.rows) {
        if (row.attribute != 6) {
            continue;
        }
        if (!row.name2 || !row.expression) {
            throw std::invalid_argument("Saved UPDATE assignment is incomplete");
        }
        sql += first ? "\nSET " : ", ";
        sql += quoted_identifier(*row.name2, true) + " = " + *row.expression;
        first = false;
    }
    if (first) {
        throw std::invalid_argument("Saved UPDATE lacks assignments");
    }
    append_where(sql, query);
}

const QueryRowInfo* cross_tab_column(const QueryInfo& query, bool pivot) {
    const QueryRowInfo* result = nullptr;
    for (const auto& row : query.rows) {
        const auto flags = row.flag.value_or(0);
        if (row.attribute == 6 && (pivot ? (flags & 1) != 0 : (flags & 3) == 0)) {
            if (result) {
                throw std::invalid_argument("Saved crosstab column is ambiguous");
            }
            result = &row;
        }
    }
    return result;
}

template <class T>
std::optional<T> read_field(const detail::PageFile& file, const detail::EncodedTableRow& row,
                            const detail::ColumnDefinition& column) {
    auto value = detail::decode_jet4_value(file, "MSysQueries", row, column);
    if (const auto* result = std::get_if<T>(&value)) {
        return *result;
    }
    if constexpr (std::is_same_v<T, std::string>) {
        if (const auto* memo = std::get_if<MemoValue>(&value)) {
            return memo->text;
        }
    }
    if (std::holds_alternative<std::monostate>(value)) {
        return std::nullopt;
    }
    DatabaseErrorContext context;
    context.table_name = "MSysQueries";
    context.column_name = column.name;
    context.row_number = row.row_number;
    throw DatabaseError(DatabaseErrorCode::corrupted_file, "Unexpected saved query field type",
                        file.path(), row.page_number, std::move(context));
}

}

std::string QueryInfo::to_sql() const {
    if (type == QueryType::data_definition || type == QueryType::passthrough) {
        const auto* row = unique_query_row(*this, 1);
        return row ? row->expression.value_or("") : std::string{};
    }
    if (type == QueryType::unknown) {
        throw std::invalid_argument("SQL reconstruction is not supported for this query type");
    }

    const auto union_part = [&](std::string_view name) {
        const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& item) {
            return item.attribute == 5 && item.name2 && *item.name2 == name;
        });
        if (row == rows.end() || !row->expression) {
            throw std::invalid_argument("Saved UNION query is missing an operand");
        }
        return clean_union_sql(*row->expression);
    };
    constexpr std::array<const char*, 21> parameter_types{
        "Value", "Bit", "Byte", "Short", "Long", "Currency", "IEEESingle", "IEEEDouble",
        "DateTime", "Binary", "Text", "LongBinary", nullptr, nullptr, nullptr, "Guid",
        nullptr, "BigBinary", "ComplexType", "BigInt", "TYPENAME",
    };
    std::string sql;
    bool has_parameters = false;
    for (const auto& row : rows) {
        if (row.attribute != 2) {
            continue;
        }
        if (!row.flag || *row.flag < 0 || static_cast<std::size_t>(*row.flag) >= parameter_types.size() ||
            !parameter_types[*row.flag] || !row.name1) {
            throw std::invalid_argument("Saved query parameter type or name is unsupported");
        }
        sql += has_parameters ? ", " : "PARAMETERS ";
        sql += *row.name1 + " " + parameter_types[*row.flag];
        if (*row.flag == 0x0a && row.extra.value_or(0) > 0) {
            sql += "(" + std::to_string(*row.extra) + ")";
        }
        has_parameters = true;
    }
    if (has_parameters) {
        sql += ";\n";
    }
    const auto* flag_row = unique_query_row(*this, 3);
    const auto flags = flag_row ? flag_row->flag.value_or(0) : 0;
    bool order_clause = true;
    switch (type) {
        case QueryType::union_query:
            sql += union_part("X7YZ_____1");
            sql += (flags & 0x02) ? "\nUNION " : "\nUNION ALL ";
            sql += union_part("X7YZ_____2");
            break;
        case QueryType::update:
            append_update(sql, *this);
            order_clause = false;
            break;
        case QueryType::append:
            if (append_insert(sql, *this)) {
                order_clause = false;
            } else {
                append_select(sql, *this, flag_row);
            }
            break;
        case QueryType::cross_tab:
            if (const auto* transform = cross_tab_column(*this, false); transform && transform->expression) {
                sql += "TRANSFORM " + *transform->expression;
                if (transform->name1) {
                    sql += " AS " + quoted_identifier(*transform->name1);
                }
                sql += '\n';
            }
            append_select(sql, *this, flag_row);
            break;
        default:
            append_select(sql, *this, flag_row);
            break;
    }

    bool has_ordering = false;
    for (const auto& row : rows) {
        if (!order_clause || row.attribute != 11) {
            continue;
        }
        if (!row.expression) {
            throw std::invalid_argument("Saved query ordering expression is missing");
        }
        sql += has_ordering ? ", " : "\nORDER BY ";
        sql += *row.expression;
        if (row.name1 == "D" || row.name1 == "d") {
            sql += " DESC";
        }
        has_ordering = true;
    }
    if (type == QueryType::cross_tab) {
        const auto* pivot = cross_tab_column(*this, true);
        if (!pivot || !pivot->expression) {
            throw std::invalid_argument("Saved crosstab lacks a pivot");
        }
        sql += "\nPIVOT " + *pivot->expression;
    }
    if ((flags & 0x04) != 0) {
        sql += "\nWITH OWNERACCESS OPTION";
    }
    sql += ';';
    return sql;
}

std::vector<QueryInfo> Database::queries() const {
    if (!state_) {
        throw std::logic_error("Database has no state; it may have been moved from");
    }
    const auto& file = state_->file;
    const auto catalog = detail::read_table_definition(file, 2);
    auto objects = detail::read_system_catalog_objects(file, catalog, std::string_view{});
    std::vector<QueryInfo> queries;
    std::map<std::int32_t, std::size_t> by_id;
    for (auto& object : objects) {
        if (object.type != 5 || !object.id || !object.name) {
            continue;
        }
        if (by_id.emplace(*object.id, queries.size()).second) {
            queries.push_back({std::move(*object.name), *object.id,
                               static_cast<std::uint32_t>(object.flags.value_or(0)), QueryType::unknown, {}});
        }
    }
    if (queries.empty()) {
        return queries;
    }

    const auto& tables = state_->schema.tables;
    const auto table = std::find_if(tables.begin(), tables.end(), [](const auto& item) {
        return !item.is_user_table && item.name == "MSysQueries";
    });
    if (table == tables.end()) {
        throw DatabaseError(DatabaseErrorCode::corrupted_file, "Missing MSysQueries catalog", file.path());
    }
    const auto column = [&](std::string_view name) -> const detail::ColumnDefinition& {
        const auto& columns = table->definition.columns;
        const auto found = std::find_if(columns.begin(), columns.end(), [&](const auto& item) {
            return item.name == name;
        });
        if (found == columns.end()) {
            throw DatabaseError(DatabaseErrorCode::corrupted_file, "Missing MSysQueries column",
                                file.path(), table->definition_page_number);
        }
        return *found;
    };
    const auto& object_id = column("ObjectId");
    const auto& attribute = column("Attribute");
    const auto& expression = column("Expression");
    const auto& flag = column("Flag");
    const auto& extra = column("LvExtra");
    const auto& name1 = column("Name1");
    const auto& name2 = column("Name2");
    const auto& order = column("Order");

    detail::TableRowReader reader(file, table->name, table->definition);
    while (const auto row = reader.next()) {
        const auto id = read_field<std::int32_t>(file, *row, object_id);
        const auto found = id ? by_id.find(*id) : by_id.end();
        if (found == by_id.end()) {
            continue;
        }
        const auto attribute_value = read_field<std::int8_t>(file, *row, attribute);
        auto order_value = read_field<BlobValue>(file, *row, order);
        queries[found->second].rows.push_back({
            attribute_value ? std::optional<std::uint8_t>(static_cast<std::uint8_t>(*attribute_value)) : std::nullopt,
            read_field<std::string>(file, *row, expression),
            read_field<std::int16_t>(file, *row, flag),
            read_field<std::int32_t>(file, *row, extra),
            read_field<std::string>(file, *row, name1),
            read_field<std::string>(file, *row, name2),
            order_value ? std::optional<std::vector<std::uint8_t>>(std::move(order_value->bytes)) : std::nullopt,
        });
    }
    for (auto& query : queries) {
        query.type = classify_query(query);
    }
    return queries;
}

}
