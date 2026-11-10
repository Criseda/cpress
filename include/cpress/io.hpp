// cpress: whole-file I/O helpers shared by the CLI, tests and benchmark.
#pragma once

#include "cpress/cpress.hpp"

#include <filesystem>

namespace cpress {

// Reads a whole regular file into memory. Throws Error on failure.
Bytes read_file(const std::filesystem::path& path);

// Writes `data` to `path`, replacing any existing file. Throws Error on failure.
void write_file(const std::filesystem::path& path, ByteView data);

}  // namespace cpress
