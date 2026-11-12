// The operating-system calls behind TableAllocator (model/table_allocator.hpp):
// large pages where the platform grants them without new privileges, and
// ordinary memory where it does not. One block per platform; each frees a
// block the same way whether or not it got large pages, so deallocation
// needs to know only the block's size.
//
//   Linux     aligned to a large page, then madvise(MADV_HUGEPAGE) before
//             the memory is first touched. Transparent huge pages set to
//             "madvise" or "always", as most distributions ship them, then
//             back the block with 2 MiB pages as they can.
//   Windows   VirtualAlloc(MEM_LARGE_PAGES), which needs the "Lock pages in
//             memory" privilege; an administrator must grant it, and it is
//             off by default. Without it, VirtualAlloc with ordinary pages.
//   Elsewhere ordinary memory.
#include "model/table_allocator.hpp"

#include <atomic>
#include <cstddef>
#include <limits>
#include <new>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <sys/mman.h>

#include <cstdlib>
#endif

namespace cpress::model::detail {
namespace {

// Set only by tests; see refuse_large_pages().
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_large_pages_refused{false};

constexpr std::align_val_t kCacheLineAlignment{kCacheLineBytes};

// `bytes` rounded up to a multiple of `unit`, a power of two. Throws
// std::bad_alloc if that does not fit in a std::size_t.
std::size_t round_up(std::size_t bytes, std::size_t unit) {
    if (bytes > std::numeric_limits<std::size_t>::max() - (unit - 1)) throw std::bad_alloc();
    return (bytes + (unit - 1)) & ~(unit - 1);
}

#ifdef _WIN32

// Holding the privilege is not enough: a process must also enable it in its
// own token before VirtualAlloc will grant large pages. That changes nothing
// outside this process, and fails, harmlessly, for an account that does not
// hold the privilege.
bool enable_lock_memory_privilege() noexcept {
    LUID lock_memory{};
    if (LookupPrivilegeValueA(nullptr, "SeLockMemoryPrivilege", &lock_memory) == 0) return false;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) == 0) {
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0] = {.Luid = lock_memory, .Attributes = SE_PRIVILEGE_ENABLED};
    // AdjustTokenPrivileges succeeds even if the account lacks the
    // privilege, and then says so in the last error.
    const bool adjusted =
        AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) != 0;
    const bool enabled = adjusted && GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return enabled;
}

// Whether large pages can be asked for: the privilege enabled, and the
// processor supporting them. Decided once.
bool large_pages_available() noexcept {
    static const bool available = GetLargePageMinimum() != 0 && enable_lock_memory_privilege();
    return available;
}

void* allocate_large(std::size_t bytes) {
    if (!g_large_pages_refused && large_pages_available()) {
        // A large-page allocation must be a whole number of large pages.
        const std::size_t rounded = round_up(bytes, GetLargePageMinimum());
        void* block = VirtualAlloc(nullptr, rounded, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES,
                                   PAGE_READWRITE);
        if (block != nullptr) return block;
        // Physical memory too fragmented for large pages: ordinary ones.
    }
    void* block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (block == nullptr) throw std::bad_alloc();
    return block;  // aligned to the allocation granularity, 64 KiB
}

void deallocate_large(void* block, std::size_t /*bytes*/) noexcept {
    VirtualFree(block, 0, MEM_RELEASE);
}

#elif defined(__linux__)

void* allocate_large(std::size_t bytes) {
    // aligned_alloc needs a whole number of the alignment, and a large page
    // can only back a whole large page.
    const std::size_t rounded = round_up(bytes, kLargePageBytes);
    void* block = std::aligned_alloc(kLargePageBytes, rounded);
    if (block == nullptr) throw std::bad_alloc();
    // A request, not a demand: if the kernel declines, or has transparent
    // huge pages off, the block keeps ordinary pages. Refused, the block is
    // kept off large pages even where the kernel would give them unasked.
    madvise(block, rounded, g_large_pages_refused ? MADV_NOHUGEPAGE : MADV_HUGEPAGE);
    return block;
}

void deallocate_large(void* block, std::size_t /*bytes*/) noexcept {
    std::free(block);  // NOLINT(cppcoreguidelines-no-malloc): from aligned_alloc
}

#else

void* allocate_large(std::size_t bytes) {
    return ::operator new(bytes, kCacheLineAlignment);
}

void deallocate_large(void* block, std::size_t bytes) noexcept {
    ::operator delete(block, bytes, kCacheLineAlignment);
}

#endif

}  // namespace

// Below kLargePageBytes a block would waste most of a large page, so it gets
// ordinary memory on every platform.
void* allocate_table(std::size_t bytes) {
    if (bytes < kLargePageBytes) return ::operator new(bytes, kCacheLineAlignment);
    return allocate_large(bytes);
}

void deallocate_table(void* block, std::size_t bytes) noexcept {
    if (bytes < kLargePageBytes) {
        ::operator delete(block, bytes, kCacheLineAlignment);
    } else {
        deallocate_large(block, bytes);
    }
}

void refuse_large_pages(bool refused) noexcept {
    g_large_pages_refused = refused;
}

}  // namespace cpress::model::detail
