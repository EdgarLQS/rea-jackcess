#include "rea_jackcess/database_file.hpp"

#include "page/page_file.hpp"

#include <utility>

namespace rea_jackcess {

struct DatabaseFile::Impl {
    explicit Impl(detail::PageFile file) : page_file(std::move(file)) {}

    detail::PageFile page_file;
};

DatabaseError::DatabaseError(DatabaseErrorCode code,
                             std::string message,
                             std::filesystem::path path,
                             std::optional<std::uint64_t> page_number)
    : std::runtime_error(std::move(message)),
      code_(code),
      path_(std::move(path)),
      page_number_(page_number) {}

DatabaseErrorCode DatabaseError::code() const noexcept {
    return code_;
}

const std::filesystem::path& DatabaseError::path() const noexcept {
    return path_;
}

std::optional<std::uint64_t> DatabaseError::page_number() const noexcept {
    return page_number_;
}

DatabaseFile::DatabaseFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

DatabaseFile::~DatabaseFile() = default;

DatabaseFile::DatabaseFile(DatabaseFile&& other) noexcept = default;

DatabaseFile& DatabaseFile::operator=(DatabaseFile&& other) noexcept = default;

DatabaseFile DatabaseFile::open(const std::filesystem::path& path) {
    return DatabaseFile(std::make_unique<Impl>(detail::PageFile::open(path)));
}

const std::filesystem::path& DatabaseFile::path() const noexcept {
    return impl_->page_file.path();
}

const FormatInfo& DatabaseFile::format() const noexcept {
    return impl_->page_file.format();
}

std::uint64_t DatabaseFile::page_count() const noexcept {
    return impl_->page_file.page_count();
}

std::vector<std::uint8_t> DatabaseFile::read_page(std::uint64_t page_number) const {
    return impl_->page_file.read_page(page_number);
}

}
