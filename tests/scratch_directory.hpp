// A directory of its own for the files a test writes.
#pragma once

#include <algorithm>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace cpress::test {

// A new directory in the temporary directory, removed with everything in it
// when this object goes.
class ScratchDirectory {
public:
    ScratchDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("cpress-tests-" + std::to_string(std::random_device{}()))) {
        std::filesystem::create_directories(path_);
    }
    // Owns a directory on disk: not copied, and removing it is the
    // destructor's job.
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    ScratchDirectory(ScratchDirectory&&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;
    // NOLINTNEXTLINE(bugprone-exception-escape): only allocation failure can throw here.
    ~ScratchDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] std::filesystem::path file(const std::string& name) const {
        return path_ / name;
    }

    // The names of the files in it, sorted.
    [[nodiscard]] std::vector<std::string> names() const {
        std::vector<std::string> found;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(path_)) {
            found.push_back(entry.path().filename().string());
        }
        std::ranges::sort(found);
        return found;
    }

private:
    std::filesystem::path path_;
};

}  // namespace cpress::test
