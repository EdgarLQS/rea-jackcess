#pragma once

#include "complex_value.hpp"
#include "ole_stream.hpp"
#include "property.hpp"
#include "row_cursor.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

// Sort-order metadata stored on a Jet4 TEXT or MEMO column definition.
struct TextSortOrder {
    std::uint16_t lcid;
    std::int8_t version;
};

class ColumnBuilder {
public:
    ColumnBuilder(std::string name, DataType type);

    // Sets the stored text sort order; indexed TEXT accepts General Legacy, General, General 97,
    // and the Jackcess 5.2.0 General Legacy fallback descriptors (LCID 1049, 1055, and 1058, version 0).
    ColumnBuilder& with_text_sort_order(std::uint16_t lcid, std::int8_t version) noexcept;
    const std::string& name() const noexcept;
    DataType type() const noexcept;
    const std::optional<TextSortOrder>& text_sort_order() const noexcept;

private:
    std::string name_;
    DataType type_;
    std::optional<TextSortOrder> text_sort_order_;
};

struct IndexColumnBuilder {
    std::string name;
    bool ascending = true;
};

class IndexBuilder {
public:
    IndexBuilder(std::string name, std::string column_name, bool unique = false,
                 bool descending = false);
    IndexBuilder(std::string name, std::vector<IndexColumnBuilder> columns, bool unique = false);

    // Declares the table primary key and implies Unique and Required.
    IndexBuilder& with_primary_key() noexcept;
    IndexBuilder& with_required(bool required = true) noexcept;
    IndexBuilder& with_ignore_nulls(bool ignore_nulls = true) noexcept;

    const std::string& name() const noexcept;
    const std::vector<IndexColumnBuilder>& columns() const noexcept;
    // Legacy accessors describe the first column of a non-empty index.
    const std::string& column_name() const noexcept;
    bool unique() const noexcept;
    bool descending() const noexcept;
    bool primary_key() const noexcept;
    bool required() const noexcept;
    bool ignore_nulls() const noexcept;

private:
    std::string name_;
    std::vector<IndexColumnBuilder> columns_;
    bool unique_;
    bool primary_key_ = false;
    bool required_ = false;
    bool ignore_nulls_ = false;
};

class TableBuilder {
public:
    explicit TableBuilder(std::string name);

    TableBuilder& add_column(ColumnBuilder column);
    TableBuilder& add_column(std::string name, DataType type);
    TableBuilder& add_index(IndexBuilder index);

    const std::string& name() const noexcept;
    const std::vector<ColumnBuilder>& columns() const noexcept;
    const std::vector<IndexBuilder>& indexes() const noexcept;

private:
    std::string name_;
    std::vector<ColumnBuilder> columns_;
    std::vector<IndexBuilder> indexes_;
};

struct ComplexColumnReference {
    std::uint32_t conceptual_table_page_number;
    std::uint32_t complex_type_table_page_number;
    std::optional<std::string> complex_type_table_name;
    // Set when the supporting type table is present in the catalog.
    std::optional<ComplexColumnType> type;
    std::uint32_t flat_table_page_number;
    std::optional<std::string> flat_table_name;
    // Keys are resolved from the flat-table definition; a missing key is reported as a database diagnostic.
    std::optional<std::string> primary_key_column_name;
    std::optional<std::string> foreign_key_column_name;
};

struct ColumnInfo {
    std::string name;
    DataType type;
    std::uint8_t raw_type_code;
    std::uint16_t size;
    std::uint8_t precision;
    std::uint8_t scale;
    bool fixed;
    // True when Jackcess' calculated-value wrapper is stored in this column.
    bool calculated = false;
    // Null when the source file did not expose a decodable Required property map.
    std::optional<bool> nullable;
    // ACE complex-column identifier from the column definition, before resolving MSysComplexColumns.
    std::optional<std::uint32_t> complex_type_id;
    // True for a flat-table column marked as the complex-value foreign key.
    bool complex_value_foreign_key = false;
    // Resolved from MSysComplexColumns when the ACE catalog has a valid matching row.
    std::optional<ComplexColumnReference> complex_column_reference;
};

// A validator may return the original value, replace it, or throw to reject a write.
using ColumnValidator = std::function<Value(const ColumnInfo&, const Value&)>;
using ColumnValidatorFactory =
    std::function<ColumnValidator(std::string_view table_name, const ColumnInfo& column)>;

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
    // True when MSysObjects marks the object hidden (flag 0x08).
    bool is_hidden() const;
    std::uint32_t row_count_header() const;
    const std::vector<ColumnInfo>& columns() const;
    const std::vector<IndexInfo>& indexes() const;
    // Overrides the database read error handler for cursors created from this Table.
    // Passing an empty handler restores the database handler.
    void set_error_handler(ErrorHandler handler);
    // Returns the stored Jet4 property blocks, including named column and index blocks.
    std::vector<PropertyMapInfo> property_maps() const;
    // Writes the raw property records for a local user table.
    void save_property_maps(const std::vector<PropertyMapInfo>& maps);
    // Creates a cursor that decodes one row at a time and can move forwards or backwards.
    RowCursor rows() const;
    // Scans only the named columns (case-insensitive), optionally skipping rows for which the
    // filter returns false. `Row::at` indexes the projected order, not the table order.
    RowCursor rows(const std::vector<std::string>& column_names, RowFilter filter = {}) const;
    // Opens the OLE payload of one row as a seekable read-only stream. The column must be an OLE
    // column and the row must hold a value for it.
    OleStream ole_stream(std::string_view column_name, const Row& row) const;

    // Reads raw rows from a complex column's flat table for the value stored in `parent_row_id`.
    // Uses a foreign-key-leading index when present; otherwise scans the flat table.
    std::vector<Row> raw_complex_values(std::string_view complex_column_name, RowId parent_row_id) const;
    // Reads typed values from a complex column; unsupported types retain their decoded raw row.
    // Attachment values include both the stored wrapper and its decoded file payload.
    std::vector<ComplexValueInfo> complex_values(std::string_view complex_column_name,
                                                RowId parent_row_id) const;
    // Scans rows in the order of the named index. The index name must match exactly.
    RowCursor rows_by_index(const std::string& index_name) const;
    RowCursor rows_by_index(const std::string& index_name,
                            const std::vector<std::string>& column_names,
                            RowFilter filter = {}) const;
    // Scans the same order starting at the first entry whose key is not less than `start`.
    // Vector bounds use all indexed columns in declaration order.
    RowCursor rows_by_index_from(const std::string& index_name, const Value& start) const;
    RowCursor rows_by_index_from(const std::string& index_name, const std::vector<Value>& start) const;
    // Scans the same order from `start` (inclusive) up to `stop` (exclusive).
    RowCursor rows_by_index_range(const std::string& index_name,
                                  const Value& start,
                                  const Value& stop) const;
    RowCursor rows_by_index_range(const std::string& index_name,
                                  const std::vector<Value>& start,
                                  const std::vector<Value>& stop) const;
    // Scans the same index from the greatest key down to the smallest.
    RowCursor rows_by_index_reverse(const std::string& index_name) const;
    // Descending scan over the same [start, stop) key interval.
    RowCursor rows_by_index_range_reverse(const std::string& index_name,
                                          const Value& start,
                                          const Value& stop) const;
    RowCursor rows_by_index_range_reverse(const std::string& index_name,
                                          const std::vector<Value>& start,
                                          const std::vector<Value>& stop) const;
    // Scans rows matching a non-empty sequence of leading index-column values.
    RowCursor rows_by_index_prefix(const std::string& index_name,
                                   const std::vector<Value>& prefix,
                                   bool reverse = false) const;
    // Returns the first row whose index key equals `key`, or nullopt when there is none.
    std::optional<Row> find_row_by_index(const std::string& index_name, const Value& key) const;
    std::optional<Row> find_row_by_index(const std::string& index_name,
                                         const std::vector<Value>& key) const;
    // Adds one row to an explicitly read-write database; returns its physical location.
    // Supported indexes on the table are updated in the same write.
    // Current Jet4 support covers BOOLEAN, fixed BYTE/SHORT/LONG/MONEY/SINGLE/DOUBLE/GUID and
    // variable TEXT/BINARY/MEMO/OLE columns. A BOOLEAN is stored in the row NULL mask, so a
    // NULL boolean is written as FALSE and cannot be distinguished after reading it back.
    RowId add_row(const std::vector<Value>& values);
    // Adds a batch of rows and returns their physical locations in input order.
    std::vector<RowId> add_rows(const std::vector<std::vector<Value>>& rows);
    // Updates local Jet4 V2000/V2003 rows in place, compacts them on shrink, or grows them
    // through overflow. RowId remains stable across these size changes, and a single-column
    // ascending index entry is moved when the indexed key changes.
    void update_row(RowId row_id, const std::vector<Value>& values);
    // Marks a live local Jet4 V2000/V2003 row as deleted and removes its index entries.
    void delete_row(RowId row_id);

    // Builds an index over the current rows of a writable local Jet4 table.
    void add_index(const IndexBuilder& index);

private:
    explicit Table(std::shared_ptr<detail::TableState> state);

    std::shared_ptr<detail::TableState> state_;

    friend class Database;
};

}
