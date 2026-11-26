#include "jet3_value.hpp"

#include "../util/code_page.hpp"

#include <variant>

namespace rea_jackcess::detail {
namespace {

constexpr std::uint8_t text_type = 0x0a;
constexpr std::uint8_t memo_type = 0x0c;

}  // namespace

Jet4Value decode_jet3_value(const PageFile& file,
                            const std::string& table_name,
                            const EncodedTableRow& row,
                            const ColumnDefinition& column) {
    if (column.column_number >= row.columns.size()) {
        throw DatabaseError(DatabaseErrorCode::corrupted_file,
                            "Decoded row does not contain the column",
                            file.path(),
                            row.page_number);
    }
    const auto& encoded = row.columns[column.column_number];
    if (column.type == 0x01 && !column.calculated) {
        // BOOLEAN lives in the row NULL mask and therefore has no column bytes.
        return decode_jet4_value(file, table_name, row, column);
    }
    if (!encoded.bytes) {
        return std::monostate{};
    }

    const std::uint16_t code_page = file.code_page_override().value_or(1252);
    if (column.type == text_type) {
        return decode_code_page_text(*encoded.bytes, code_page);
    }
    if (column.type == memo_type) {
        if (encoded.bytes->empty()) {
            return std::monostate{};
        }
        const auto raw = read_long_value_bytes(file, table_name, row, column);
        return MemoValue{decode_code_page_text(raw, code_page), raw};
    }

    // Numbers, dates, GUIDs, binary data, BOOLEAN and the OLE container share the Jet4 layout.
    return decode_jet4_value(file, table_name, row, column);
}

}  // namespace rea_jackcess::detail
