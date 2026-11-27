#pragma once

#include "rea_jackcess/database_file.hpp"

#include <filesystem>

namespace rea_jackcess::detail {

// A new database built in a temporary file inside a staging directory. It becomes visible at the
// target path only when publish() moves it into place; destroying it without publishing discards it.
class StagedDatabaseFile {
public:
    ~StagedDatabaseFile();

    StagedDatabaseFile(const StagedDatabaseFile&) = delete;
    StagedDatabaseFile& operator=(const StagedDatabaseFile&) = delete;
    StagedDatabaseFile(StagedDatabaseFile&&) = delete;
    StagedDatabaseFile& operator=(StagedDatabaseFile&&) = delete;

    // Path of the temporary file that holds the in-progress database.
    const std::filesystem::path& temporary_path() const noexcept;
    // Moves the temporary file into the target path (replacing it) and marks it published.
    void publish();

private:
    StagedDatabaseFile(std::filesystem::path target_path,
                       std::filesystem::path staging_directory,
                       std::filesystem::path temporary_path);

    friend StagedDatabaseFile stage_jet4_database(const std::filesystem::path&, DatabaseFormat);

    std::filesystem::path target_path_;          // Final destination path.
    std::filesystem::path staging_directory_;    // Directory holding the temporary file.
    std::filesystem::path temporary_path_;       // Temporary in-progress file.
    bool published_ = false;                     // True once publish() has run.
};

// Creates (but does not publish) a new empty Jet4 database of `format` staged next to `target_path`.
StagedDatabaseFile stage_jet4_database(const std::filesystem::path& target_path, DatabaseFormat format);

}
