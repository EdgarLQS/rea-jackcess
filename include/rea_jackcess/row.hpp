#pragma once

#include "value.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rea_jackcess {

class RowCursor;

struct RowId {
    // Physical row location, ordered by page then row number.
    std::uint64_t page_number = 0;
    std::uint16_t row_number = 0;

    friend constexpr bool operator==(RowId left, RowId right) noexcept {
        return left.page_number == right.page_number && left.row_number == right.row_number;
    }

    friend constexpr bool operator<(RowId left, RowId right) noexcept {
        return left.page_number < right.page_number ||
               (left.page_number == right.page_number && left.row_number < right.row_number);
    }
};

class Row {
public:
    const RowId& id() const noexcept;
    std::size_t size() const noexcept;
    const std::vector<Value>& values() const noexcept;
    const Value& at(std::size_t column_index) const;

private:
    Row(RowId id, std::vector<Value> values);

    RowId id_;
    std::vector<Value> values_;

    friend class RowCursor;
    friend class Table;
};

}
