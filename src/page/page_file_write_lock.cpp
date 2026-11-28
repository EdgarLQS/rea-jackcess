#include "page_file_write_lock.hpp"

#include "rea_jackcess/database_file.hpp"

#include <cerrno>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace rea_jackcess::detail {
namespace {

// Result of one open-and-lock attempt; `failure` keeps the reason so the caller can report the same
// errors on every platform.
struct LockAttempt {
    enum class Failure { none, open, conflict, lock };

    int descriptor = -1;
    Failure failure = Failure::none;
};

#ifdef _WIN32

constexpr int write_lock_open_flags = _O_RDWR | _O_BINARY | _O_NOINHERIT;

std::filesystem::path lock_path_for(const std::filesystem::path& database) {
    auto path = database;
    path += ".reajlock";
    return path;
}

// A byte-range lock on the database would also block this process's own page reads, so the lock
// lives in a sidecar opened with deny-share: any second writer fails to open it, while the database
// itself stays readable and writable.
LockAttempt open_and_lock(const std::filesystem::path& path, int flags) {
    static_cast<void>(flags);
    LockAttempt attempt;

    const int probe = ::_wopen(path.c_str(), write_lock_open_flags);
    if (probe < 0) {
        attempt.failure = LockAttempt::Failure::open;
        return attempt;
    }
    ::_close(probe);

    const auto lock_path = lock_path_for(path);
    const int descriptor = ::_wsopen(lock_path.c_str(),
                                     _O_RDWR | _O_CREAT | _O_BINARY | _O_NOINHERIT,
                                     _SH_DENYRW,
                                     _S_IREAD | _S_IWRITE);
    if (descriptor < 0) {
        attempt.failure = (errno == EACCES || errno == EPERM) ? LockAttempt::Failure::conflict
                                                             : LockAttempt::Failure::lock;
        return attempt;
    }

    attempt.descriptor = descriptor;
    return attempt;
}

void close_locked_file(int descriptor) { ::_close(descriptor); }

void remove_lock_file(const std::filesystem::path& path) noexcept {
    if (path.empty()) {
        return;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

#else

constexpr int write_lock_open_flags = O_RDWR | O_CLOEXEC;

LockAttempt open_and_lock(const std::filesystem::path& path, int flags) {
    LockAttempt attempt;

    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        attempt.failure = LockAttempt::Failure::open;
        return attempt;
    }

    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        const int lock_error = errno;
        ::close(descriptor);
        attempt.failure = (lock_error == EWOULDBLOCK || lock_error == EAGAIN)
                              ? LockAttempt::Failure::conflict
                              : LockAttempt::Failure::lock;
        return attempt;
    }

    attempt.descriptor = descriptor;
    return attempt;
}

void close_locked_file(int descriptor) { ::close(descriptor); }

#endif

}  // namespace

PageFileWriteLock::PageFileWriteLock(int descriptor, std::filesystem::path lock_path) noexcept
    : descriptor_(descriptor), lock_path_(std::move(lock_path)) {}

PageFileWriteLock PageFileWriteLock::acquire(const std::filesystem::path& path) {
    return acquire_with_flags(path,
                              write_lock_open_flags,
                              "Cannot open database file for read-write access");
}

PageFileWriteLock PageFileWriteLock::acquire_for_replacement(const std::filesystem::path& path) {
    return acquire_with_flags(path,
                              write_lock_open_flags,
                              "Cannot open existing database destination for write locking");
}

PageFileWriteLock PageFileWriteLock::acquire_with_flags(const std::filesystem::path& path,
                                                        int flags,
                                                        const char* open_error_message) {
    const LockAttempt attempt = open_and_lock(path, flags);
    switch (attempt.failure) {
        case LockAttempt::Failure::none:
            break;
        case LockAttempt::Failure::open:
            throw DatabaseError(DatabaseErrorCode::io_error, open_error_message, path);
        case LockAttempt::Failure::conflict:
            throw DatabaseError(DatabaseErrorCode::io_error,
                                "Database already has an active read-write handle",
                                path);
        case LockAttempt::Failure::lock:
            throw DatabaseError(DatabaseErrorCode::io_error,
                                "Cannot acquire an exclusive database write lock",
                                path);
    }

#ifdef _WIN32
    return PageFileWriteLock(attempt.descriptor, lock_path_for(path));
#else
    return PageFileWriteLock(attempt.descriptor, {});
#endif
}

PageFileWriteLock::PageFileWriteLock(PageFileWriteLock&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)),
      lock_path_(std::move(other.lock_path_)) {
    other.lock_path_.clear();
}

PageFileWriteLock& PageFileWriteLock::operator=(PageFileWriteLock&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
    descriptor_ = std::exchange(other.descriptor_, -1);
    lock_path_ = std::move(other.lock_path_);
    other.lock_path_.clear();
    return *this;
}

PageFileWriteLock::~PageFileWriteLock() {
    release();
}

void PageFileWriteLock::release() noexcept {
    if (descriptor_ >= 0) {
        close_locked_file(descriptor_);
        descriptor_ = -1;
    }
#ifdef _WIN32
    remove_lock_file(lock_path_);
    lock_path_.clear();
#endif
}

}
