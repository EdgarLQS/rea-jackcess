#pragma once

#include "row.hpp"

#include <memory>
#include <optional>

namespace rea_jackcess {

class Table;

class RowCursor {
public:
    ~RowCursor();

    RowCursor(RowCursor&& other) noexcept;
    RowCursor& operator=(RowCursor&& other) noexcept;

    RowCursor(const RowCursor&) = delete;
    RowCursor& operator=(const RowCursor&) = delete;

    // Returns the next row, or nullopt after the last row or on a moved-from cursor.
    std::optional<Row> next();

private:
    struct Impl;
    explicit RowCursor(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;

    friend class Table;
};

}
