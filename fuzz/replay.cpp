// cpress-fuzz-replay: runs the decoder's fuzz target on files, with any
// compiler. libFuzzer comes with Clang only; this reproduces an input it
// found under MSVC or GCC, and ctest runs it on the fuzzer's seeds.
//
//   cpress-fuzz-replay <file or directory>...
//
// A directory stands for the regular files in it, in name order. A bug
// aborts, as it does under libFuzzer. An input the target skips tests
// nothing, so asking for one fails the run.
#include "fuzz_target.hpp"

#include "cpress/cpress.hpp"
#include "cpress/io.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <vector>

namespace {

namespace fs = std::filesystem;

// The files `argument` stands for.
std::vector<fs::path> inputs_of(const fs::path& argument) {
    if (!fs::is_directory(argument)) return {argument};
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(argument)) {
        if (entry.is_regular_file()) files.push_back(entry.path());
    }
    std::ranges::sort(files);
    return files;
}

// Runs the target on `file`; returns true if it was tested, not skipped.
bool replay(const fs::path& file) {
    const cpress::Bytes archive = cpress::read_file(file);
    if (LLVMFuzzerTestOneInput(archive.data(), archive.size()) == cpress::fuzz::kSkipped) {
        std::cerr << "skipped: " << file.string() << "\n";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // argc can be 0: the standard does not promise a program name.
    const std::span<const char* const> arguments(argv, static_cast<std::size_t>(argc));
    if (arguments.size() < 2) {
        std::cerr << "usage: cpress-fuzz-replay <file or directory>...\n";
        return 2;
    }
    try {
        std::size_t tested = 0;
        std::size_t skipped = 0;
        for (const char* argument : arguments.subspan(1)) {
            for (const fs::path& file : inputs_of(argument)) {
                ++(replay(file) ? tested : skipped);
            }
        }
        std::cout << tested << " inputs tested, " << skipped << " skipped\n";
        return tested > 0 && skipped == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "cpress-fuzz-replay: " << e.what() << "\n";
        return 2;
    }
}
