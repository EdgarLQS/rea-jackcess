#pragma once

#include "value.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rea_jackcess {

class RowCursor;

// Physical location of a row: the page it lives on and its slot within that page. Ordered by page
// then row number, and stable across row updates that grow or shrink the row's stored size.
struct RowId {
    // Zero-based page number inside the database file.
    std::uint64_t page_number = 0;
    // Zero-based position of the row within the page's row directory.
    std::uint16_t row_number = 0;

    friend constexpr bool operator==(RowId left, RowId right) noexcept {
        return left.page_number == right.page_number && left.row_number == right.row_number;
    }

    friend constexpr bool operator<(RowId left, RowId right) noexcept {
        return left.page_number < right.page_number ||
               (left.page_number == right.page_number && left.row_number < right.row_number);
    }
};

// One decoded row: its physical location plus the cell values in projection order.
class Row {
public:
    // Physical location of this row (usable with RowCursor::move_to and Table::update_row/delete_row).
    const RowId& id() const noexcept;
    // Number of cells in this row (the table's column count, or the projection's column count).
    std::size_t size() const noexcept;
    // The decoded cells in projection order; `std::monostate` is a NULL cell.
    const std::vector<Value>& values() const noexcept;
    // The cell at `column_index` (in projection order, not necessarily the table's own order).
    // Throws std::out_of_range when `column_index` is not less than size().
    const Value& at(std::size_t column_index) const;

private:
    Row(RowId id, std::vector<Value> values);

    RowId id_;
    std::vector<Value> values_;

    friend class RowCursor;
    friend class Table;
};

}
