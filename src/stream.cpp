#include "cpress/stream.hpp"

#include "checked_stream.hpp"
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

namespace cpress {
namespace {

namespace fs = std::filesystem;

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

// Creates a directory beside `output` for the output to be written in
// until it is complete, and returns its path: `output` with ".cpress-tmp"
// after it, and a number after that if the name is taken. Creating a
// directory fails if anything has its name, so the directory is this call's
// alone: two runs never write to the same temporary file, and nothing that
// was there is touched.
fs::path create_directory_beside(const fs::path& output) {
    constexpr int kTries = 100;
    for (int i = 0; i < kTries; ++i) {
        fs::path directory = output;
        directory += ".cpress-tmp";
        if (i > 0) directory += std::to_string(i);
        std::error_code ec;
        if (fs::create_directory(directory, ec)) return directory;
        // A name is taken by anything, a broken symbolic link included.
        std::error_code ignored;
        if (!fs::exists(fs::symlink_status(directory, ignored))) {
            throw Error("cannot write '" + output.string() + "': " + ec.message());
        }
    }
    throw Error("cannot find a free temporary name beside '" + output.string() + "'");
}

// The file an output is written as until it is complete, in a directory of
// its own beside the output. Both are removed when it goes out of scope:
// after a failure the file is still there, and after rename_to() only the
// empty directory is.
class TemporaryOutput {
public:
    explicit TemporaryOutput(const fs::path& output)
        : directory_(create_directory_beside(output)), file_(directory_ / "partial") {}
    // Owns a directory on disk: not copied, and removing it is the
    // destructor's job.
    TemporaryOutput(const TemporaryOutput&) = delete;
    TemporaryOutput& operator=(const TemporaryOutput&) = delete;
    TemporaryOutput(TemporaryOutput&&) = delete;
    TemporaryOutput& operator=(TemporaryOutput&&) = delete;
    ~TemporaryOutput() {
        std::error_code ignored;
        fs::remove(file_, ignored);
        fs::remove(directory_, ignored);
    }

    [[nodiscard]] const fs::path& file() const noexcept {
        return file_;
    }

    // Moves the complete file to `output`, replacing the file there rather
    // than writing into it.
    void rename_to(const fs::path& output) const {
        std::error_code ec;
        fs::rename(file_, output, ec);
        if (ec) throw Error("cannot write '" + output.string() + "': " + ec.message());
    }

private:
    fs::path directory_;
    fs::path file_;
};

[[noreturn]] void throw_changed_size(const fs::path& input) {
    throw Error("'" + input.string() + "' changed size while it was being read");
}

}  // namespace

// The archive is written as a temporary file and renamed at the end, so a
// compression that fails leaves any file that was at `output` as it was.
void compress_file(const fs::path& input, const fs::path& output, const Codec& codec) {
    const std::uint64_t size = detail::size_of_regular_file(input);
    const TemporaryOutput temporary(output);
    {
        FileSink archive(temporary.file());
        // The CRC-32 of the input is not known until it has been read, so
        // the header goes out with a zero in its place, which is put right
        // at the end.
        archive.write(detail::header_bytes(codec.id, size, 0));
        std::uint32_t crc = 0;
        if (codec.encode_stream != nullptr) {
            detail::FileSource source(input);
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
    temporary.rename_to(output);
}

void decompress(ByteView archive, ByteSink& output) {
    const Header header = read_header(archive);
    const Codec& codec = detail::codec_of(header);
    const ByteView payload = archive.subspan(kHeaderSize);

    detail::ChecksumSink checked(output);
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
    const TemporaryOutput temporary(output);
    {
        FileSink data(temporary.file());
        decompress(bytes, data);
        data.close();
    }
    temporary.rename_to(output);
}

}  // namespace cpress
