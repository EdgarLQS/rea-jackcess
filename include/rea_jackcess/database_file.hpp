#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace rea_jackcess {

enum class EngineVersion {
    jet3,
    jet4,
    ace2007,
    ace2010,
    ace2016,
    ace2019,
    msisam,
};

enum class FileFormat {
    v1997,
    generic_jet4,
    v2000,
    v2003,
    v2007,
    v2010,
    v2016,
    v2019,
    msisam,
};

enum class DatabaseFormat {
    v2000,
    v2003,
};

enum class DatabaseErrorCode {
    io_error,
    truncated_file,
    unsupported_version,
    corrupted_file,
    page_out_of_range,
    unsupported_operation,
    // A requested write would break a constraint the file already declares, so the
    // operation is rejected without changing any page.
    constraint_violation,
};

struct DatabaseErrorContext {
    std::optional<std::string> table_name;
    std::optional<std::uint64_t> row_number;
    std::optional<std::uint64_t> column_number;
    std::optional<std::string> column_name;
};

class DatabaseError : public std::runtime_error {
public:
    DatabaseError(DatabaseErrorCode code,
                  std::string message,
                  std::filesystem::path path = {},
                  std::optional<std::uint64_t> page_number = std::nullopt,
                  DatabaseErrorContext context = {});

    DatabaseErrorCode code() const noexcept;
    const std::filesystem::path& path() const noexcept;
    std::optional<std::uint64_t> page_number() const noexcept;
    const DatabaseErrorContext& context() const noexcept;

private:
    DatabaseErrorCode code_;
    std::filesystem::path path_;
    std::optional<std::uint64_t> page_number_;
    DatabaseErrorContext context_;
};

struct FormatInfo {
    EngineVersion version;
    std::uint32_t page_size;
    bool read_only;

    const char* name() const noexcept;
};

class DatabaseFile {
public:
    // Opens the file read-only and identifies its engine version from the header.
    static DatabaseFile open(const std::filesystem::path& path);

    ~DatabaseFile();

    DatabaseFile(DatabaseFile&& other) noexcept;
    DatabaseFile& operator=(DatabaseFile&& other) noexcept;

    DatabaseFile(const DatabaseFile&) = delete;
    DatabaseFile& operator=(const DatabaseFile&) = delete;

    const std::filesystem::path& path() const noexcept;
    const FormatInfo& format() const noexcept;
    // For Jet4, inspects the database AccessVersion property in addition to the header.
    FileFormat file_format() const;
    std::uint64_t page_count() const noexcept;

    // Returns stored page bytes. Page zero is not unmasked and encoded pages are not decoded.
    std::vector<std::uint8_t> read_page(std::uint64_t page_number) const;

private:
    struct Impl;
    explicit DatabaseFile(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}
