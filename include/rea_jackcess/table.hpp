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

struct ColumnInfo {
    std::string name;
    DataType type;
    std::uint8_t raw_type_code;
    std::uint16_t size;
    std::uint8_t precision;
    std::uint8_t scale;
    bool fixed;
    std::optional<bool> nullable;
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
    // Creates a single-pass cursor that decodes one row at a time.
    RowCursor rows() const;

private:
    explicit Table(std::shared_ptr<detail::TableState> state);

    std::shared_ptr<detail::TableState> state_;

    friend class Database;
};

}
