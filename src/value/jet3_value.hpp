#pragma once

#include "jet4_value.hpp"

namespace rea_jackcess::detail {

// Jet3 shares the value layout for numbers, dates, GUIDs, binary data, BOOLEAN, OLE and MEMO
// containers. Only text differs: Jet3 stores single-byte code-page text instead of UTF-16LE.
Jet4Value decode_jet3_value(const PageFile& file,
                            const std::string& table_name,
                            const EncodedTableRow& row,
                            const ColumnDefinition& column);

}  // namespace rea_jackcess::detail
