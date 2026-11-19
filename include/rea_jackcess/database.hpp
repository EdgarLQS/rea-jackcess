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
