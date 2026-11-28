#pragma once

#include "table.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace rea_jackcess {

struct CsvOptions {
    // Export writes a name record first; import reads the names from it and matches them to the
    // target table case-insensitively.
    bool header = false;
    char delimiter = ',';
    char quote = '"';

    // Export only: an empty list selects every column; otherwise columns are emitted in this
    // order. Names are resolved case-insensitively and headers use the stored column names.
    std::vector<std::string> columns;
    // Export only: called with each projected row; false skips that row. Import ignores this.
    std::function<bool(const Row&)> row_filter;
    // Import only: called with parsed source fields before mapping/conversion; false skips the row.
    // A header record is consumed before this predicate is called.
    std::function<bool(const std::vector<std::string>&)> import_row_filter;
};

// Writes one record per row using the text form the fixed Jackcess 5.2.0 ExportUtil produces:
// an empty field for NULL, true/false for BOOLEAN, four fractional digits for MONEY,
// Java-style floating point text, braced upper-case GUIDs, and upper-case hex bytes grouped in
// fours for BINARY and OLE. Complex columns export the decimal ComplexValueForeignKey reference
// for multi-value, attachment, version-history and unsupported complex types; their flat-table
// values are not expanded. TEXT and MEMO are quoted only when they contain the delimiter, the
// quote character, CR or LF, and embedded quotes are doubled. Complex columns can be exported but
// not imported; other unsupported column types raise DatabaseErrorCode::unsupported_operation.
void export_table_csv(const Table& table, std::ostream& output, const CsvOptions& options = {});
void export_table_csv(const Table& table,
                      const std::filesystem::path& path,
                      const CsvOptions& options = {});

// Appends every record to the table and returns the number of rows that were written. An empty
// field becomes NULL; fields are converted by the target column type. With options.header the
// first record names the columns and must only name columns the table owns; columns the header
// omits are written as NULL.
// Records are appended in chunks, so a conversion failure can leave earlier chunks in the table.
std::size_t import_table_csv(Table& table, std::istream& input, const CsvOptions& options = {});
std::size_t import_table_csv(Table& table,
                             const std::filesystem::path& path,
                             const CsvOptions& options = {});

}
