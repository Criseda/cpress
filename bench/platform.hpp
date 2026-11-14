// Small OS layer for the benchmark runner: peak memory, large pages, child
// processes, processor affinity.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace platform {

// Peak resident memory of this process so far, in bytes: on Windows the
// peak working set, which never includes memory on large pages.
std::uint64_t peak_memory_bytes();

// Whether peak_memory_bytes() leaves out memory on large pages: true on
// Windows, where it is the working set.
bool peak_memory_omits_large_pages();

// The bytes of transparent huge pages the kernel has handed out on a page
// fault so far, to any process: thp_fault_alloc in /proc/vmstat, in 2 MiB
// pages. 0 where there is no such count, as everywhere but Linux.
std::uint64_t huge_page_bytes_faulted();

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

// Stops standard output translating line endings, as it does on Windows,
// so that bytes written to it arrive as they were written.
void make_stdout_binary();

// The shell's name for the null device, for discarding stderr in capture().
extern const char* const kNullDevice;

}  // namespace platform
