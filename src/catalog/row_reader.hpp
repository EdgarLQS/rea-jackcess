#pragma once

#include "table_definition.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

// One column's raw bytes as laid out in the row, before type decoding. A BOOLEAN carries its
// value as a bit, so it is reported separately (`boolean_value`) with no byte payload.
struct EncodedColumnValue {
    // Decoded BOOLEAN bit (only meaningful for BOOLEAN columns).
    bool boolean_value = false;
    // Raw column bytes (absent for BOOLEAN and for NULL cells).
    std::optional<std::vector<std::uint8_t>> bytes;
};

// One row split into per-column raw values, located at a physical position.
struct EncodedTableRow {
    // Page number of the row.
    std::uint64_t page_number;
    // Row slot within the page.
    std::uint16_t row_number;
    // One entry per column, in definition order.
    std::vector<EncodedColumnValue> columns;
};

// Splits one row's `bytes` into per-column raw values according to `definition` (fixed offsets
// plus the variable-length jump table). `table_name` is used only for error context.
EncodedTableRow decode_row_layout(const PageFile& file,
                                 const std::string& table_name,
                                 std::uint64_t page_number,
                                 std::uint16_t row_number,
                                 const std::vector<std::uint8_t>& bytes,
                                 const TableDefinition& definition);

// Iterates the data rows of one table, splitting each into per-column raw values.
class TableRowReader {
public:
    // `table_name` is for diagnostics; `definition` supplies column layout and the page usage map.
    TableRowReader(const PageFile& file,
                   std::string table_name,
                   const TableDefinition& definition);
    ~TableRowReader();

    TableRowReader(TableRowReader&& other) noexcept;
    TableRowReader& operator=(TableRowReader&& other) noexcept;

    TableRowReader(const TableRowReader&) = delete;
    TableRowReader& operator=(const TableRowReader&) = delete;

    // Returns the next row in table order, or nullopt at the end.
    std::optional<EncodedTableRow> next();
    // Reads the row at an explicit physical location (used for index-directed lookups).
    EncodedTableRow read_at(std::uint64_t page_number, std::uint16_t row_number) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Reads every data row of `definition`'s table into a vector (convenience over TableRowReader).
std::vector<EncodedTableRow> read_table_rows(const PageFile& file,
                                             const std::string& table_name,
                                             const TableDefinition& definition);

}
