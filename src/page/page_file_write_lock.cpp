#include "page_file_write_lock.hpp"

#include "rea_jackcess/database_file.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <utility>

namespace rea_jackcess::detail {

PageFileWriteLock::PageFileWriteLock(int descriptor) noexcept : descriptor_(descriptor) {}

PageFileWriteLock PageFileWriteLock::acquire(const std::filesystem::path& path) {
    return acquire_with_flags(path,
                              O_RDWR | O_CLOEXEC,
                              "Cannot open database file for read-write access");
}

PageFileWriteLock PageFileWriteLock::acquire_for_replacement(const std::filesystem::path& path) {
    return acquire_with_flags(path,
                              O_RDWR | O_CLOEXEC,
                              "Cannot open existing database destination for write locking");
}

PageFileWriteLock PageFileWriteLock::acquire_with_flags(const std::filesystem::path& path,
                                                        int flags,
                                                        const char* open_error_message) {
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        throw DatabaseError(DatabaseErrorCode::io_error, open_error_message, path);
    }

    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        const int lock_error = errno;
        ::close(descriptor);
        if (lock_error == EWOULDBLOCK || lock_error == EAGAIN) {
            throw DatabaseError(DatabaseErrorCode::io_error,
                                "Database already has an active read-write handle",
                                path);
        }
        throw DatabaseError(DatabaseErrorCode::io_error,
                            "Cannot acquire an exclusive database write lock",
                            path);
    }

    return PageFileWriteLock(descriptor);
}

PageFileWriteLock::PageFileWriteLock(PageFileWriteLock&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)) {}

PageFileWriteLock& PageFileWriteLock::operator=(PageFileWriteLock&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
    descriptor_ = std::exchange(other.descriptor_, -1);
    return *this;
}

PageFileWriteLock::~PageFileWriteLock() {
    release();
}

void PageFileWriteLock::release() noexcept {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
}

}
