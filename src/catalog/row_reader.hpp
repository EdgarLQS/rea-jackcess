#pragma once

#include "table_definition.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rea_jackcess::detail {

struct EncodedColumnValue {
    bool boolean_value = false;
    std::optional<std::vector<std::uint8_t>> bytes;
};

struct EncodedTableRow {
    std::uint64_t page_number;
    std::uint16_t row_number;
    std::vector<EncodedColumnValue> columns;
};

class TableRowReader {
public:
    TableRowReader(const PageFile& file,
                   std::string table_name,
                   const TableDefinition& definition);
    ~TableRowReader();

    TableRowReader(TableRowReader&& other) noexcept;
    TableRowReader& operator=(TableRowReader&& other) noexcept;

    TableRowReader(const TableRowReader&) = delete;
    TableRowReader& operator=(const TableRowReader&) = delete;

    std::optional<EncodedTableRow> next();
    EncodedTableRow read_at(std::uint64_t page_number, std::uint16_t row_number) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::vector<EncodedTableRow> read_table_rows(const PageFile& file,
                                             const std::string& table_name,
                                             const TableDefinition& definition);

}
