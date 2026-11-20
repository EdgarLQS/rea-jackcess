#pragma once

#include "database_file.hpp"
#include "table.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rea_jackcess {

namespace detail {
struct DatabaseState;
}

struct DatabaseDiagnostic {
    std::optional<std::string> table_name;
    std::string message;
    std::optional<std::uint64_t> page_number;
};

enum class DatabaseOpenMode {
    read_only,
    read_write,
};

// Accessors on a moved-from Database throw std::logic_error.
class Database {
public:
    // Opens an existing Jet4 database read-only. Other formats remain unsupported here.
    static Database open(const std::filesystem::path& path);

    // Opens an existing Jet4 database with the requested access mode.
    // Read-write handles hold an exclusive advisory lock until all dependent handles are closed.
    static Database open(const std::filesystem::path& path, DatabaseOpenMode mode);

    // Creates a Jet4 database, then opens it for reading.
    // Replacing an existing file fails while it has an active read-write handle.
    // Concurrent calls to either create overload targeting the same path must be serialized by the caller.
    static Database create(const std::filesystem::path& path, DatabaseFormat format);

    // Creates a Jet4 V2000 database with the supplied empty table schemas, then opens it for reading.
    // Current schema writing supports BYTE/SHORT/LONG/SINGLE/DOUBLE/TEXT/BINARY columns whose definition fits one page
    // (TEXT defaults to 255 UTF-16 characters and BINARY to 255 bytes), and 1-64 character ASCII names
    // containing letters, digits, spaces, or underscores.
    // The returned Database handle remains read-only.
    static Database create(const std::filesystem::path& path,
                           DatabaseFormat format,
                           const std::vector<TableBuilder>& tables);

    const std::filesystem::path& path() const;
    const FormatInfo& format() const;
    // Diagnostics include tables that could not be described, such as linked tables.
    const std::vector<DatabaseDiagnostic>& diagnostics() const;
    // Returns local tables discovered in the database, including system tables.
    std::vector<Table> tables() const;

private:
    explicit Database(std::shared_ptr<detail::DatabaseState> state);

    std::shared_ptr<detail::DatabaseState> state_;
};

}
