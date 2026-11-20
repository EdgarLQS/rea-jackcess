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

// Accessors on a moved-from Database throw std::logic_error.
class Database {
public:
    // Opens an existing Jet4 database read-only. Other formats remain unsupported here.
    static Database open(const std::filesystem::path& path);

    // Creates a Jet4 database, then opens it for reading.
    // Concurrent calls to either create overload targeting the same path must be serialized by the caller.
    static Database create(const std::filesystem::path& path, DatabaseFormat format);

    // Creates a Jet4 V2000 database with the supplied empty table schemas, then opens it for reading.
    // Current schema writing supports LONG columns whose definition fits one page, and 1-64 character ASCII names
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
