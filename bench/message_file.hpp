// A file of ITCH messages read a message at a time, for the tools that go
// through a day file: it is gigabytes, and they need a message of it at
// once. The reading is the message codec's own: its look-ahead window over
// the streaming functions' file source.
#pragma once

#include "checked_stream.hpp"
#include "codec/input_window.hpp"
#include "cpress/cpress.hpp"
#include "cpress/stream.hpp"
#include "itch/schema.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace cpress::bench {

class MessageFile {
public:
    // Throws cpress::Error if the file is not a regular file or cannot be
    // opened.
    explicit MessageFile(const std::filesystem::path& path)
        : source_(path), window_(source_, kBufferBytes) {}

    // The next message, with its length prefix, as itch::framed_message_at()
    // sees it. The bytes are good until the next call. Empty if what comes
    // next is not a message: the end of the file, or bytes that are not
    // ITCH, at which the position stays. Throws cpress::Error if a read
    // fails.
    [[nodiscard]] ByteView next() {
        const ByteView ahead = window_.ahead();
        const itch::MessageLayout* layout = itch::framed_message_at(ahead);
        if (layout == nullptr) return {};
        const ByteView message = ahead.first(itch::kLengthPrefixBytes + layout->length);
        window_.advance(message.size());
        return message;
    }

    // True once next() has gone through the whole file: it was messages
    // and nothing else.
    [[nodiscard]] bool at_end() const noexcept {
        return window_.at_end();
    }

    // Reads whatever is left without looking at it, so that bytes_read()
    // and crc32() are of the whole file.
    void read_rest() {
        for (ByteView ahead = window_.ahead(); !ahead.empty(); ahead = window_.ahead()) {
            window_.advance(ahead.size());
        }
    }

    // How much of the file has been read so far, and the CRC-32 of that.
    [[nodiscard]] std::uint64_t bytes_read() const noexcept {
        return source_.bytes();
    }
    [[nodiscard]] std::uint32_t crc32() const noexcept {
        return source_.crc();
    }

private:
    static constexpr std::size_t kBufferBytes = std::size_t{1} << 20;

    // The file, which fails, rather than ends, if it gives other than the
    // bytes its size said it held. That is how a failed read shows, since
    // the stream can report one as the end of the file.
    class WholeFile final : public ByteSource {
    public:
        explicit WholeFile(const std::filesystem::path& path)
            : path_(path), size_(detail::size_of_regular_file(path)), file_(path) {}

        std::size_t read(std::span<std::uint8_t> buffer) override {
            const std::size_t count = file_.read(buffer);
            if (count == 0 && file_.bytes() != size_) {
                throw Error("read " + std::to_string(file_.bytes()) + " bytes of '" +
                            path_.string() + "', whose size is " + std::to_string(size_) +
                            ": a read failed or the file changed");
            }
            return count;
        }

        [[nodiscard]] std::uint64_t bytes() const noexcept {
            return file_.bytes();
        }
        [[nodiscard]] std::uint32_t crc() const noexcept {
            return file_.crc();
        }

    private:
        std::filesystem::path path_;
        std::uint64_t size_;
        detail::FileSource file_;
    };

    WholeFile source_;
    codec::detail::InputWindow window_;  // reads source_
};

}  // namespace cpress::bench
