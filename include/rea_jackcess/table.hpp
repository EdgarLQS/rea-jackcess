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

// Logical column type, mapped from the stored Jet/ACE type code.
enum class DataType {
    boolean,          // TRUE/FALSE, stored in the row NULL mask (1 bit).
    byte,             // 1-byte signed integer.
    int16,            // 2-byte signed integer (Jet SHORT).
    int32,            // 4-byte signed integer (Jet LONG).
    money,            // Currency: 64-bit scaled decimal with four fractional digits (DecimalValue).
    float32,          // 4-byte IEEE single (SINGLE).
    float64,          // 8-byte IEEE double (DOUBLE).
    short_date_time,  // Jet DATE serial stored as a double -> DateTimeValue.
    binary,           // Fixed/max-length binary -> BlobValue.
    text,             // Short text, stored UTF-16 in the file, exposed as UTF-8 std::string.
    ole,              // OLE object payload -> BlobValue.
    memo,             // Long text -> MemoValue.
    guid,             // 16-byte GUID -> GuidValue.
    numeric,          // Exact decimal with precision/scale (NUMERIC).
    big_binary,       // Large binary (OLE / long binary).
    complex,          // ACE complex column (multi-value, attachment, version history).
    int64,            // 8-byte signed integer (ACE LONG LONG).
    extended_date_time, // ACE EXT_DATE_TIME -> DateTimeValue.
    unknown,          // A type code this library does not map.
};

// Sort-order metadata stored on a Jet4 TEXT or MEMO column definition.
struct TextSortOrder {
    // Windows locale id selecting the collation (e.g. 1033 = en-US).
    std::uint16_t lcid;
    // Sort-version selector (e.g. 0 = legacy/general, 1 = General, -1 = General 97).
    std::int8_t version;
};

// Declares one column of a table being created.
class ColumnBuilder {
public:
    // `name` is an ASCII identifier (letters, digits, spaces, or underscores, 1-64 characters);
    // `type` is the logical column type to create.
    ColumnBuilder(std::string name, DataType type);

    // Sets the stored text sort order; indexed TEXT accepts General Legacy, General, General 97,
    // and the Jackcess 5.2.0 General Legacy fallback descriptors (LCID 1049, 1055, and 1058, version 0).
    ColumnBuilder& with_text_sort_order(std::uint16_t lcid, std::int8_t version) noexcept;
    // The declared column name.
    const std::string& name() const noexcept;
    // The declared logical type.
    DataType type() const noexcept;
    // The text sort order for TEXT/MEMO columns, or nullopt when none was set.
    const std::optional<TextSortOrder>& text_sort_order() const noexcept;

private:
    std::string name_;
    DataType type_;
    std::optional<TextSortOrder> text_sort_order_;
};

// One column of a multi-column index definition: which column and its sort direction.
struct IndexColumnBuilder {
    // Name of the indexed column.
    std::string name;
    // true = ascending, false = descending, for this column.
    bool ascending = true;
};

// Declares one index over a table being created (or added later via Table::add_index).
class IndexBuilder {
public:
    // Single-column convenience overload. `name` is the index name, `column_name` the indexed
    // column, `unique` forbids duplicate non-NULL keys, `descending` sets the sort direction.
    IndexBuilder(std::string name, std::string column_name, bool unique = false,
                 bool descending = false);
    // Multi-column overload: `columns` lists the key columns in order with per-column direction.
    IndexBuilder(std::string name, std::vector<IndexColumnBuilder> columns, bool unique = false);

    // Declares the table primary key and implies Unique and Required.
    IndexBuilder& with_primary_key() noexcept;
    // Marks the index Required; the optional flag (default true) sets the state.
    IndexBuilder& with_required(bool required = true) noexcept;
    // Ignores rows whose key is entirely NULL for uniqueness purposes; optional enable flag.
    IndexBuilder& with_ignore_nulls(bool ignore_nulls = true) noexcept;

    // The index name.
    const std::string& name() const noexcept;
    // The key columns in declaration order.
    const std::vector<IndexColumnBuilder>& columns() const noexcept;
    // Legacy accessors describe the first column of a non-empty index.
    const std::string& column_name() const noexcept;
    // Whether the index forbids duplicate non-NULL keys (also true for a primary key).
    bool unique() const noexcept;
    // Whether the first key column is descending (false for an empty index).
    bool descending() const noexcept;
    // Whether this is the table primary key.
    bool primary_key() const noexcept;
    // Whether the index is Required (also true for a primary key).
    bool required() const noexcept;
    // Whether all-NULL keys are ignored for uniqueness.
    bool ignore_nulls() const noexcept;

private:
    std::string name_;
    std::vector<IndexColumnBuilder> columns_;
    bool unique_;
    bool primary_key_ = false;
    bool required_ = false;
    bool ignore_nulls_ = false;
};

// Declares a table to create: its name, columns and initial indexes.
class TableBuilder {
public:
    // `name` is an ASCII table identifier (same rules as a column name).
    explicit TableBuilder(std::string name);

    // Appends a fully built column.
    TableBuilder& add_column(ColumnBuilder column);
    // Appends a column by name and logical type (convenience overload).
    TableBuilder& add_column(std::string name, DataType type);
    // Appends an index definition.
    TableBuilder& add_index(IndexBuilder index);

    // The table name.
    const std::string& name() const noexcept;
    // The declared columns, in declaration order.
    const std::vector<ColumnBuilder>& columns() const noexcept;
    // The declared indexes.
    const std::vector<IndexBuilder>& indexes() const noexcept;

private:
    std::string name_;
    std::vector<ColumnBuilder> columns_;
    std::vector<IndexBuilder> indexes_;
};

// How an ACE complex column links across its conceptual, type and flat tables.
struct ComplexColumnReference {
    // Page of the user-visible "conceptual" table that owns this complex column.
    std::uint32_t conceptual_table_page_number;
    // Page of the complex-type table (holds the column's metadata rows).
    std::uint32_t complex_type_table_page_number;
    // Name of the complex-type table when it is present in the catalog.
    std::optional<std::string> complex_type_table_name;
    // Classified complex type, set when the supporting type table is present in the catalog.
    std::optional<ComplexColumnType> type;
    // Page of the flat table that stores the per-value rows.
    std::uint32_t flat_table_page_number;
    // Name of the flat table, if known.
    std::optional<std::string> flat_table_name;
    // Keys are resolved from the flat-table definition; a missing key is reported as a database diagnostic.
    std::optional<std::string> primary_key_column_name;
    std::optional<std::string> foreign_key_column_name;
};

// One column of a table, as read from the table definition.
struct ColumnInfo {
    // Column name.
    std::string name;
    // Logical type mapped from raw_type_code (for a calculated column, its result type).
    DataType type;
    // The stored Jet/ACE type code before mapping (e.g. 0x04 = LONG, 0x12 = COMPLEX).
    std::uint8_t raw_type_code;
    // Declared fixed/variable storage length in bytes (or a text length hint for TEXT).
    std::uint16_t size;
    // Decimal precision for NUMERIC columns; 0 otherwise.
    std::uint8_t precision;
    // Decimal scale (fractional digits) for NUMERIC columns; 0 otherwise.
    std::uint8_t scale;
    // True for a fixed-length column, false for a variable-length one.
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

// A validator may return the original value, replace it, or throw to reject a write. It receives
// the target column's info and the value being written and returns the value to store.
using ColumnValidator = std::function<Value(const ColumnInfo&, const Value&)>;
// Builds a per-column validator for a table/column; returning an empty validator keeps the
// column's default behavior.
using ColumnValidatorFactory =
    std::function<ColumnValidator(std::string_view table_name, const ColumnInfo& column)>;

// One key column of an index.
struct IndexColumnInfo {
    // Name of the indexed column.
    std::string name;
    // 1-based column number within the table's definition.
    std::uint16_t column_number;
    // Sort direction for this key column (true = ascending).
    bool ascending;
};

// One index (logical definition plus its physical B-tree) of a table.
struct IndexInfo {
    // Index name.
    std::string name;
    // Logical index number within the table definition.
    std::uint32_t index_number;
    // Physical index (B-tree) number this logical index points at; several logical indexes may share one.
    std::uint32_t physical_index_number;
    // The key columns in index order.
    std::vector<IndexColumnInfo> columns;
    // Page number of the index's root B-tree page.
    std::uint32_t root_page_number;
    // Count of distinct keys (unique-entry count) stored with the physical index.
    std::uint32_t unique_entry_count;
    // Raw physical index flags byte (0x01 unique, 0x02 ignore-nulls, 0x08 required).
    std::uint8_t flags;
    // Unique: rejects duplicate non-NULL keys (also true for a primary-key index).
    bool unique;
    // Rows with an entirely NULL key are ignored for uniqueness.
    bool ignore_nulls;
    // Index marked Required.
    bool required;
    // This index is the table primary key.
    bool primary_key;
    // This index is a foreign-key index (referenced columns are populated below).
    bool foreign_key;
    // Referenced index type for a foreign key (1 = primary-key index, 2 = foreign-key index).
    std::optional<std::uint8_t> referenced_index_type;
    // Logical index number of the index on the referenced (parent) table.
    std::optional<std::uint32_t> referenced_index_number;
    // Page number of the referenced parent table.
    std::optional<std::uint32_t> referenced_table_page_number;
    // Foreign-key cascade actions declared on the index.
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
    // As above, but decodes only `column_names` (case-insensitive, projection order) and skips
    // rows for which `filter` returns false (an empty filter keeps all rows).
    RowCursor rows_by_index(const std::string& index_name,
                            const std::vector<std::string>& column_names,
                            RowFilter filter = {}) const;
    // Scans the same order starting at the first entry whose key is not less than `start`.
    // The vector overload supplies one value per indexed column (in declaration order) for a
    // composite key; the scalar overload is the single-column shorthand.
    RowCursor rows_by_index_from(const std::string& index_name, const Value& start) const;
    RowCursor rows_by_index_from(const std::string& index_name, const std::vector<Value>& start) const;
    // Scans the same order from `start` (inclusive) up to `stop` (exclusive). A scalar key applies
    // to the index's first column; a vector supplies the full composite key.
    RowCursor rows_by_index_range(const std::string& index_name,
                                  const Value& start,
                                  const Value& stop) const;
    RowCursor rows_by_index_range(const std::string& index_name,
                                  const std::vector<Value>& start,
                                  const std::vector<Value>& stop) const;
    // Scans the same index from the greatest key down to the smallest.
    RowCursor rows_by_index_reverse(const std::string& index_name) const;
    // Descending scan over the same [start, stop) key interval (bounds as in rows_by_index_range).
    RowCursor rows_by_index_range_reverse(const std::string& index_name,
                                          const Value& start,
                                          const Value& stop) const;
    RowCursor rows_by_index_range_reverse(const std::string& index_name,
                                          const std::vector<Value>& start,
                                          const std::vector<Value>& stop) const;
    // Scans rows matching a non-empty sequence of leading index-column values (`prefix`, one entry
    // per leading column); `reverse` scans descending. The prefix is an index lookup, not a thrown
    // condition: no matches yields an empty cursor. Throws unsupported_operation when the encoded
    // prefix would exceed the maximum index-key length.
    RowCursor rows_by_index_prefix(const std::string& index_name,
                                   const std::vector<Value>& prefix,
                                   bool reverse = false) const;
    // Returns the first row whose index key equals `key`, or nullopt when there is none. The
    // vector overload is for composite keys.
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
