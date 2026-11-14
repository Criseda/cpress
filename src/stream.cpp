#include "cpress/stream.hpp"

#include "container.hpp"
#include "cpress/cpress.hpp"
#include "cpress/io.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace cpress {
namespace {

namespace fs = std::filesystem;

// A file read in order. Sums the CRC-32 of what it has given and counts it,
// since a streamed input is seen only once.
class FileSource final : public ByteSource {
public:
    explicit FileSource(const fs::path& path) : path_(path), file_(path, std::ios::binary) {
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
    fs::path path_;
    std::ifstream file_;
    std::uint32_t crc_ = 0;
    std::uint64_t bytes_ = 0;
};

// A file written in order, replacing any that was there.
class FileSink final : public ByteSink {
public:
    explicit FileSink(const fs::path& path)
        : path_(path), file_(path, std::ios::binary | std::ios::trunc) {
        if (!file_) throw Error("cannot create '" + path.string() + "'");
    }

    void write(ByteView bytes) override {
        file_.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        if (!file_) fail();
    }

    // Overwrites bytes already written, `offset` from the start, and leaves
    // the file ready to be closed.
    void rewrite(std::uint64_t offset, ByteView bytes) {
        file_.seekp(static_cast<std::streamoff>(offset));
        write(bytes);
    }

    // Flushes and closes. Throws Error if anything was not written.
    void close() {
        file_.close();
        if (!file_) fail();
    }

private:
    [[noreturn]] void fail() const {
        throw Error("write to '" + path_.string() + "' failed");
    }

    fs::path path_;
    std::ofstream file_;
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

// Removes a file when it goes out of scope, unless keep() was called: for
// output that must not be left behind half written.
class RemoveUnlessKept {
public:
    explicit RemoveUnlessKept(fs::path path) : path_(std::move(path)) {}
    // Owns the duty to remove one file: not copied, and the destructor is
    // the point of it.
    RemoveUnlessKept(const RemoveUnlessKept&) = delete;
    RemoveUnlessKept& operator=(const RemoveUnlessKept&) = delete;
    RemoveUnlessKept(RemoveUnlessKept&&) = delete;
    RemoveUnlessKept& operator=(RemoveUnlessKept&&) = delete;
    // Only a file: if the output could not be created because the name is
    // a directory's, the directory is not this object's to remove.
    ~RemoveUnlessKept() {
        if (kept_) return;
        std::error_code ignored;
        if (fs::is_regular_file(path_, ignored)) fs::remove(path_, ignored);
    }

    void keep() noexcept {
        kept_ = true;
    }

private:
    fs::path path_;
    bool kept_ = false;
};

std::uint64_t size_of_regular_file(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        throw Error("cannot read '" + path.string() + "': not a regular file");
    }
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) throw Error("cannot read '" + path.string() + "': " + ec.message());
    return size;
}

// A name beside `output` that no file has, for the output to be written
// under until it is complete: `output` with ".cpress-tmp" after it, and a
// number after that if a file already has the name.
fs::path temporary_beside(const fs::path& output) {
    constexpr int kTries = 100;
    for (int i = 0; i < kTries; ++i) {
        fs::path temporary = output;
        temporary += ".cpress-tmp";
        if (i > 0) temporary += std::to_string(i);
        std::error_code ec;
        if (!fs::exists(temporary, ec) && !ec) return temporary;
    }
    throw Error("cannot find a free temporary name beside '" + output.string() + "'");
}

// Moves a complete file into place, replacing any file there.
void rename_into_place(const fs::path& temporary, const fs::path& output) {
    std::error_code ec;
    fs::rename(temporary, output, ec);
    if (ec) throw Error("cannot write '" + output.string() + "': " + ec.message());
}

[[noreturn]] void throw_changed_size(const fs::path& input) {
    throw Error("'" + input.string() + "' changed size while it was being read");
}

}  // namespace

// The archive is written under a temporary name and renamed at the end, so
// a compression that fails leaves any file that was at `output` as it was.
void compress_file(const fs::path& input, const fs::path& output, const Codec& codec) {
    const std::uint64_t size = size_of_regular_file(input);
    const fs::path temporary = temporary_beside(output);
    RemoveUnlessKept guard(temporary);
    {
        FileSink archive(temporary);
        // The CRC-32 of the input is not known until it has been read, so
        // the header goes out with a zero in its place, which is put right
        // at the end.
        archive.write(detail::header_bytes(codec.id, size, 0));
        std::uint32_t crc = 0;
        if (codec.encode_stream != nullptr) {
            FileSource source(input);
            codec.encode_stream(source, size, archive);
            if (source.bytes() != size) throw_changed_size(input);
            crc = source.crc();
        } else {
            const Bytes data = read_file(input);
            if (data.size() != size) throw_changed_size(input);
            Bytes payload;
            codec.encode(data, payload);
            archive.write(payload);
            crc = crc32(data);
        }
        const auto header = detail::header_bytes(codec.id, size, crc);
        archive.rewrite(detail::kCrcOffset,
                        std::span(header).subspan(detail::kCrcOffset, detail::kCrcBytes));
        archive.close();
    }
    rename_into_place(temporary, output);
    guard.keep();
}

void decompress(ByteView archive, ByteSink& output) {
    const Header header = read_header(archive);
    const Codec& codec = detail::codec_of(header);
    const ByteView payload = archive.subspan(kHeaderSize);

    ChecksumSink checked(output);
    if (codec.decode_stream != nullptr) {
        codec.decode_stream(payload, header.original_size, checked);
    } else {
        if (header.original_size > std::numeric_limits<std::size_t>::max()) {
            throw Error("archive is too large for this platform");
        }
        Bytes data;
        codec.decode(payload, header.original_size, data);
        checked.write(data);
    }
    if (checked.bytes() != header.original_size) {
        throw Error("corrupt archive: decoded size does not match the header");
    }
    if (checked.crc() != header.crc32) {
        throw Error("corrupt archive: checksum mismatch");
    }
}

void decompress_file(const fs::path& archive, const fs::path& output) {
    const Bytes bytes = read_file(archive);
    const fs::path temporary = temporary_beside(output);
    RemoveUnlessKept guard(temporary);
    {
        FileSink data(temporary);
        decompress(bytes, data);
        data.close();
    }
    rename_into_place(temporary, output);
    guard.keep();
}

}  // namespace cpress
