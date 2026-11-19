#pragma once

#include "value.hpp"

#include <cstddef>
#include <vector>

namespace rea_jackcess {

class RowCursor;

class Row {
public:
    std::size_t size() const noexcept;
    const std::vector<Value>& values() const noexcept;
    const Value& at(std::size_t column_index) const;

private:
    explicit Row(std::vector<Value> values);

    std::vector<Value> values_;

    friend class RowCursor;
};

}
