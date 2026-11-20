#pragma once

#include <filesystem>

namespace rea_jackcess::detail {

class PageFileWriteLock {
public:
    static PageFileWriteLock acquire(const std::filesystem::path& path);
    static PageFileWriteLock acquire_for_replacement(const std::filesystem::path& path);

    PageFileWriteLock(const PageFileWriteLock&) = delete;
    PageFileWriteLock& operator=(const PageFileWriteLock&) = delete;
    PageFileWriteLock(PageFileWriteLock&& other) noexcept;
    PageFileWriteLock& operator=(PageFileWriteLock&& other) noexcept;
    ~PageFileWriteLock();

private:
    explicit PageFileWriteLock(int descriptor) noexcept;
    static PageFileWriteLock acquire_with_flags(const std::filesystem::path& path,
                                                int flags,
                                                const char* open_error_message);
    void release() noexcept;

    int descriptor_ = -1;
};

}
