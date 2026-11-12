// Small OS layer for the benchmark runner: peak memory, large pages, child
// processes, processor affinity.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace platform {

// Peak memory of this process so far, in bytes: resident memory, or on
// Windows the larger of the working set and the committed memory, since
// Windows never counts large pages in a working set.
std::uint64_t peak_memory_bytes();

// What the operating system has counted of large pages, read before a
// measurement so that large_page_bytes() can compare. On Linux, the
// transparent huge pages it has handed out on a page fault, to any process
// (/proc/vmstat); Windows keeps no such count, and needs none.
struct LargePageCount {
    std::uint64_t pages = 0;
};
LargePageCount count_large_pages();

// How many bytes of each model built since `before` were on large pages,
// where `models` were built one after another, never two at once; nothing
// where the platform cannot tell. Linux: the huge pages handed out since
// `before`, shared out among the models; another process taking huge pages
// meanwhile would inflate it. Windows: the process's peak committed memory
// less its peak working set, which is the memory on large pages of the one
// model alive at the time, give or take memory committed and never touched.
std::optional<std::uint64_t> large_page_bytes(LargePageCount before, int models);

// Restricts this process to logical processor `cpu`, so the scheduler
// cannot move it to a slower core mid-run. Returns false if that processor
// does not exist or the platform cannot pin (macOS has no such call).
bool pin_to_cpu(int cpu);

// Absolute path of the running executable. Falls back to `argv0`.
std::filesystem::path self_path(const char* argv0);

// Runs `exe` with `args`, waits for it and returns its exit code (-1 if it
// could not be started). The child shares this process's stdout and stderr.
int run_process(const std::filesystem::path& exe, const std::vector<std::string>& args);

// Runs a shell command and returns its stdout with trailing whitespace
// removed. Returns an empty string if the command could not be run.
std::string capture(const std::string& command);

// The shell's name for the null device, for discarding stderr in capture().
extern const char* const kNullDevice;

}  // namespace platform
