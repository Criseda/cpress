// Small OS layer for the benchmark runner: peak memory, child processes.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace platform {

// Peak resident memory of this process so far, in bytes.
std::uint64_t peak_memory_bytes();

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
