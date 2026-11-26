#pragma once

#include "jet3_value.hpp"
#include "jet4_value.hpp"

namespace rea_jackcess::detail {

// Dispatches to the value layout of the file's engine version.
inline Jet4Value decode_value(const PageFile& file,
                              const std::string& table_name,
                              const EncodedTableRow& row,
                              const ColumnDefinition& column) {
    if (file.format().version == EngineVersion::jet3) {
        return decode_jet3_value(file, table_name, row, column);
    }
    return decode_jet4_value(file, table_name, row, column);
}

}  // namespace rea_jackcess::detail
