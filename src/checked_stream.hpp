// A file read in order and a sink that passes bytes on, each counting what
// goes through it and summing its CRC-32: how the streaming functions check
// what they read and write, and how the benchmark tools read a day file.
// Not part of the public API.
#pragma once

#include "cpress/cpress.hpp"
#include "cpress/stream.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <system_error>

namespace cpress::detail {

// The size of `path`. Throws Error unless it is a regular file.
[[nodiscard]] inline std::uint64_t size_of_regular_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        throw Error("cannot read '" + path.string() + "': not a regular file");
    }
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) throw Error("cannot read '" + path.string() + "': " + ec.message());
    return size;
}

// A file read in order. Sums the CRC-32 of what it has given and counts it,
// since a streamed input is seen only once.
//
// A stream can report a failed read as the end of the file (libc++ does,
// reading a directory), so a caller that must have read the whole file
// compares bytes() with the size the file had.
class FileSource final : public ByteSource {
public:
    explicit FileSource(const std::filesystem::path& path)
        : path_(path), file_(path, std::ios::binary) {
        if (!file_) throw Error("cannot open '" + path.string() + "'");
    }

    std::size_t read(std::span<std::uint8_t> buffer) override {
        file_.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(file_.gcount());
        if (file_.bad()) throw Error("read from '" + path_.string() + "' failed");
        crc_ = crc32(buffer.first(count), crc_);
        bytes_ += count;
        return count;
    }

    [[nodiscard]] std::uint32_t crc() const noexcept {
        return crc_;
    }
    [[nodiscard]] std::uint64_t bytes() const noexcept {
        return bytes_;
    }

private:
    std::filesystem::path path_;
    std::ifstream file_;
    std::uint32_t crc_ = 0;
    std::uint64_t bytes_ = 0;
};

// Passes bytes on to another sink, summing their CRC-32 and counting them.
class ChecksumSink final : public ByteSink {
public:
    explicit ChecksumSink(ByteSink& next) : next_(&next) {}

    void write(ByteView bytes) override {
        crc_ = crc32(bytes, crc_);
        bytes_ += bytes.size();
        next_->write(bytes);
    }

    [[nodiscard]] std::uint32_t crc() const noexcept {
        return crc_;
    }
    [[nodiscard]] std::uint64_t bytes() const noexcept {
        return bytes_;
    }

private:
    ByteSink* next_;  // not owned
    std::uint32_t crc_ = 0;
    std::uint64_t bytes_ = 0;
};

}  // namespace cpress::detail
