#pragma once

#include "row.hpp"

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace rea_jackcess {

class Table;
struct ColumnInfo;

// A row predicate applied while scanning. Returning false skips the row, which keeps the cursor
// navigation (previous/first/last/move_to) working over the matching rows only.
using RowFilter = std::function<bool(const Row&)>;
using RowPattern = std::vector<std::pair<std::string, Value>>;
using ColumnMatcher = std::function<bool(
    std::string_view table_name,
    std::string_view column_name,
    const Value& expected,
    const Value& actual)>;

// Handles a column decoding failure by throwing again or returning a replacement value.
// An empty handler rethrows the original exception.
using ErrorHandler = std::function<Value(
    std::string_view table_name,
    const ColumnInfo& column,
    const std::optional<std::vector<std::uint8_t>>& column_data,
    RowId row_id,
    const std::exception& error)>;

class RowCursor {
public:
    ~RowCursor();

    RowCursor(RowCursor&& other) noexcept;
    RowCursor& operator=(RowCursor&& other) noexcept;

    RowCursor(const RowCursor&) = delete;
    RowCursor& operator=(const RowCursor&) = delete;

    // Returns the next row, or nullopt after the last row or on a moved-from cursor.
    std::optional<Row> next();
    // Returns the row before the current position, or nullopt when already at the first row.
    std::optional<Row> previous();
    std::optional<Row> first();
    std::optional<Row> last();
    // Positions the cursor on the row with this id and returns it. Rows outside the cursor's
    // scan order or filtered out are not found, so a RowId from position() is a savepoint.
    std::optional<Row> move_to(RowId row_id);
    // Row id of the last returned row, suitable for move_to() later.
    std::optional<RowId> position() const;
    // Re-reads the row at the current position without moving.
    std::optional<Row> current();
    // Checks the current row against the named values; an empty pattern matches any positioned row.
    bool current_row_matches(const RowPattern& pattern);
    // Searches from the beginning of this cursor's scan order and positions on the first match.
    std::optional<Row> find_first_row(const RowPattern& pattern);
    // Searches after the current position and positions on the next match.
    std::optional<Row> find_next_row(const RowPattern& pattern);
    // Overrides the inherited table/database read error handler; empty restores inheritance.
    void set_error_handler(ErrorHandler handler);
    // Installs value comparison for row-pattern matching; empty restores the cursor's default matcher.
    void set_column_matcher(ColumnMatcher matcher);

private:
    struct Impl;
    explicit RowCursor(std::unique_ptr<Impl> impl);

    // Row only befriends RowCursor, so the decode helpers build rows through this seam.
    static Row make_row(RowId id, std::vector<Value> values);

    std::unique_ptr<Impl> impl_;

    friend class Table;
};

}
