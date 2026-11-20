#pragma once

#include "rea_jackcess/database_file.hpp"

#include <filesystem>

namespace rea_jackcess::detail {

class StagedDatabaseFile {
public:
    ~StagedDatabaseFile();

    StagedDatabaseFile(const StagedDatabaseFile&) = delete;
    StagedDatabaseFile& operator=(const StagedDatabaseFile&) = delete;
    StagedDatabaseFile(StagedDatabaseFile&&) = delete;
    StagedDatabaseFile& operator=(StagedDatabaseFile&&) = delete;

    const std::filesystem::path& temporary_path() const noexcept;
    void publish();

private:
    StagedDatabaseFile(std::filesystem::path target_path,
                       std::filesystem::path staging_directory,
                       std::filesystem::path temporary_path);

    friend StagedDatabaseFile stage_jet4_database(const std::filesystem::path&, DatabaseFormat);

    std::filesystem::path target_path_;
    std::filesystem::path staging_directory_;
    std::filesystem::path temporary_path_;
    bool published_ = false;
};

StagedDatabaseFile stage_jet4_database(const std::filesystem::path& target_path, DatabaseFormat format);

}
