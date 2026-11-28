#pragma once

#include <filesystem>

namespace rea_jackcess::detail {

// Exclusive OS advisory lock on a database file, held for the lifetime of a read-write handle so
// cooperating processes cannot both open it for writing. Released on destruction.
//
// POSIX locks the database file itself with `flock`. Windows cannot hold a byte-range lock on the
// database without also blocking this process's own page reads, so it locks a `<database>.reajlock`
// sidecar opened with deny-share instead; the sidecar is deleted on release.
class PageFileWriteLock {
public:
    // Acquires an exclusive lock on an existing `path` (for in-place writes).
    static PageFileWriteLock acquire(const std::filesystem::path& path);
    // Acquires a lock prepared for replacing `path` (a new file published over it).
    static PageFileWriteLock acquire_for_replacement(const std::filesystem::path& path);

    PageFileWriteLock(const PageFileWriteLock&) = delete;
    PageFileWriteLock& operator=(const PageFileWriteLock&) = delete;
    PageFileWriteLock(PageFileWriteLock&& other) noexcept;
    PageFileWriteLock& operator=(PageFileWriteLock&& other) noexcept;
    ~PageFileWriteLock();

private:
    PageFileWriteLock(int descriptor, std::filesystem::path lock_path) noexcept;
    static PageFileWriteLock acquire_with_flags(const std::filesystem::path& path,
                                                int flags,
                                                const char* open_error_message);
    void release() noexcept;

    int descriptor_ = -1;  // Locked descriptor, or -1 when no lock is held.
    // Windows sidecar removed on release; empty on POSIX, where the database itself is locked.
    std::filesystem::path lock_path_;
};

}
