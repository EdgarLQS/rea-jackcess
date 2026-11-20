#pragma once

#include "row_cursor.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rea_jackcess {

namespace detail {
struct TableState;
}

enum class DataType {
    boolean,
    byte,
    int16,
    int32,
    money,
    float32,
    float64,
    short_date_time,
    binary,
    text,
    ole,
    memo,
    guid,
    numeric,
    big_binary,
    complex,
    int64,
    extended_date_time,
    unknown,
};

class ColumnBuilder {
public:
    ColumnBuilder(std::string name, DataType type);

    const std::string& name() const noexcept;
    DataType type() const noexcept;

private:
    std::string name_;
    DataType type_;
};

class TableBuilder {
public:
    explicit TableBuilder(std::string name);

    TableBuilder& add_column(ColumnBuilder column);
    TableBuilder& add_column(std::string name, DataType type);

    const std::string& name() const noexcept;
    const std::vector<ColumnBuilder>& columns() const noexcept;

private:
    std::string name_;
    std::vector<ColumnBuilder> columns_;
};

struct ColumnInfo {
    std::string name;
    DataType type;
    std::uint8_t raw_type_code;
    std::uint16_t size;
    std::uint8_t precision;
    std::uint8_t scale;
    bool fixed;
    // Null when the source file did not expose a decodable Required property map.
    std::optional<bool> nullable;
};

struct IndexColumnInfo {
    std::string name;
    std::uint16_t column_number;
    bool ascending;
};

struct IndexInfo {
    std::string name;
    std::uint32_t index_number;
    std::uint32_t physical_index_number;
    std::vector<IndexColumnInfo> columns;
    std::uint32_t root_page_number;
    std::uint32_t unique_entry_count;
    std::uint8_t flags;
    bool unique;
    bool ignore_nulls;
    bool required;
    bool primary_key;
    bool foreign_key;
    std::optional<std::uint8_t> referenced_index_type;
    std::optional<std::uint32_t> referenced_index_number;
    std::optional<std::uint32_t> referenced_table_page_number;
    bool cascade_updates;
    bool cascade_deletes;
    bool cascade_set_null;
};

// Accessors on a moved-from Table throw std::logic_error.
class Table {
public:
    const std::string& name() const;
    const std::string& object_type() const;
    bool is_user_table() const;
    bool is_linked() const;
    std::uint32_t row_count_header() const;
    const std::vector<ColumnInfo>& columns() const;
    const std::vector<IndexInfo>& indexes() const;
    // Creates a single-pass cursor that decodes one row at a time.
    RowCursor rows() const;
    // Scans rows in the order of the named index. The index name must match exactly.
    RowCursor rows_by_index(const std::string& index_name) const;
    // Adds one row to an explicitly read-write database; returns its physical location.
    RowId add_row(const std::vector<Value>& values);
    // Adds a batch of rows and returns their physical locations in input order.
    std::vector<RowId> add_rows(const std::vector<std::vector<Value>>& rows);

private:
    explicit Table(std::shared_ptr<detail::TableState> state);

    std::shared_ptr<detail::TableState> state_;

    friend class Database;
};

}
